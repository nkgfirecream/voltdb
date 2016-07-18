/* This file is part of VoltDB.
 * Copyright (C) 2008-2016 VoltDB Inc.
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU Affero General Public License as
 * published by the Free Software Foundation, either version 3 of the
 * License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Affero General Public License for more details.
 *
 * You should have received a copy of the GNU Affero General Public License
 * along with VoltDB.  If not, see <http://www.gnu.org/licenses/>.
 */

#include "MaterializedViewHandler.h"
#include "catalog/column.h"
#include "catalog/statement.h"
#include "catalog/table.h"
#include "catalog/tableref.h"
#include "common/executorcontext.hpp"
#include "indexes/tableindex.h"
#include "TableCatalogDelegate.hpp"


ENABLE_BOOST_FOREACH_ON_CONST_MAP(TableRef);
ENABLE_BOOST_FOREACH_ON_CONST_MAP(Statement);
typedef std::pair<std::string, catalog::TableRef*> LabeledTableRef;
typedef std::pair<std::string, catalog::Statement*> LabeledStatement;

namespace voltdb {

    MaterializedViewHandler::MaterializedViewHandler(PersistentTable *destTable,
                                                     catalog::MaterializedViewHandlerInfo *mvHandlerInfo,
                                                     VoltDBEngine *engine) {
        install(destTable, mvHandlerInfo, engine);
        setUpAggregateInfo(mvHandlerInfo);
        setUpCreateQuery(mvHandlerInfo, engine);
        setUpMinMaxQueries(mvHandlerInfo, engine);
        setUpBackedTuples();
        m_dirty = false;
        catchUpWithExistingData();
    }

    MaterializedViewHandler::~MaterializedViewHandler() {
#ifdef VOLT_TRACE_ENABLED
        if (ExecutorContext::getExecutorContext()->m_siteId == 0)
            cout << m_destTable->name() << " MaterializedViewHandler::~MaterializedViewHandler() " << endl;
#endif
        for (int i=m_sourceTables.size()-1; i>=0; i--) {
            dropSourceTable(m_sourceTables[i]);
        }
    }

    void MaterializedViewHandler::addSourceTable(PersistentTable *sourceTable) {
#ifdef VOLT_TRACE_ENABLED
        if (ExecutorContext::getExecutorContext()->m_siteId == 0)
            cout << m_destTable->name() << " MaterializedViewHandler::addSourceTable() " << sourceTable->name()  << endl;
#endif
        sourceTable->addViewToTrigger(this);
        m_sourceTables.push_back(sourceTable);
        m_dirty = true;
    }

    void MaterializedViewHandler::dropSourceTable(PersistentTable *sourceTable) {
#ifdef VOLT_TRACE_ENABLED
        if (ExecutorContext::getExecutorContext()->m_siteId == 0)
            cout << m_destTable->name() << " MaterializedViewHandler::dropSourceTable() " << sourceTable->name()  << endl;
#endif
        assert( ! m_sourceTables.empty());
        sourceTable->dropViewToTrigger(this);
        PersistentTable* lastTable = m_sourceTables.back();
        if (sourceTable != lastTable) {
            // iterator to vector element:
            std::vector<PersistentTable*>::iterator it = find(m_sourceTables.begin(),
                                                              m_sourceTables.end(),
                                                              sourceTable);
            assert(it != m_sourceTables.end());
            // Use the last view to patch the potential hole.
            *it = lastTable;
        }
        // The last element is now excess.
        m_sourceTables.pop_back();
        m_dirty = true;
    }

    void MaterializedViewHandler::install(PersistentTable *destTable,
                                          catalog::MaterializedViewHandlerInfo *mvHandlerInfo,
                                          VoltDBEngine *engine) {
        m_destTable = destTable;
        m_index = m_destTable->primaryKeyIndex();
        const std::vector<TableIndex*>& targetIndexes = m_destTable->allIndexes();
        BOOST_FOREACH(TableIndex *index, targetIndexes) {
            if (index != m_index) {
                m_updatableIndexList.push_back(index);
            }
        }
        m_groupByColumnCount = mvHandlerInfo->groupByColumnCount();
        // Delete the existing handler if exists. When the existing handler is destructed,
        // it will automatically removes itself from all the viewsToTrigger lists of its source tables.
        delete m_destTable->m_mvHandler;
        // The handler will not only be installed on the view table, but also the source tables.
        m_destTable->m_mvHandler = this;
        BOOST_FOREACH (LabeledTableRef labeledTableRef, mvHandlerInfo->sourceTables()) {
            catalog::TableRef *sourceTableRef = labeledTableRef.second;
            TableCatalogDelegate *sourceTcd =  engine->getTableDelegate(sourceTableRef->table()->name());
            PersistentTable *sourceTable = sourceTcd->getPersistentTable();
            assert(sourceTable);
            addSourceTable(sourceTable);
        }
    }

    void MaterializedViewHandler::setUpAggregateInfo(catalog::MaterializedViewHandlerInfo *mvHandlerInfo) {
        const catalog::CatalogMap<catalog::Column>& columns = mvHandlerInfo->destTable()->columns();
        m_aggColumnCount = columns.size() - m_groupByColumnCount - 1;
        m_aggTypes.resize(m_aggColumnCount);
        for (catalog::CatalogMap<catalog::Column>::field_map_iter colIterator = columns.begin();
                colIterator != columns.end(); colIterator++) {
            const catalog::Column *destCol = colIterator->second;
            if (destCol->index() < m_groupByColumnCount + 1) {
                continue;
            }
            // The index into the per-agg metadata starts as a materialized view column index
            // but needs to be shifted down for each column that has no agg option
            // -- that is, -1 for each "group by" AND -1 for the COUNT(*).
            std::size_t aggIndex = destCol->index() - m_groupByColumnCount - 1;
            m_aggTypes[aggIndex] = static_cast<ExpressionType>(destCol->aggregatetype());
            switch(m_aggTypes[aggIndex]) {
                case EXPRESSION_TYPE_AGGREGATE_SUM:
                case EXPRESSION_TYPE_AGGREGATE_COUNT:
                case EXPRESSION_TYPE_AGGREGATE_MIN:
                case EXPRESSION_TYPE_AGGREGATE_MAX:
                    break; // legal value
                default: {
                    char message[128];
                    snprintf(message, 128, "Error in materialized view aggregation %d expression type %s",
                             (int)aggIndex, expressionToString(m_aggTypes[aggIndex]).c_str());
                    throw SerializableEEException(VOLT_EE_EXCEPTION_TYPE_EEEXCEPTION,
                                                  message);
                }
            }
        }
    }

    void MaterializedViewHandler::setUpCreateQuery(catalog::MaterializedViewHandlerInfo *mvHandlerInfo,
                                                      VoltDBEngine *engine) {
        catalog::Statement *createQueryStatement = mvHandlerInfo->createQuery().get("createQuery");
        m_createQueryExecutorVector = ExecutorVector::fromCatalogStatement(engine, createQueryStatement);
        m_createQueryExecutorVector->getRidOfSendExecutor();
#ifdef VOLT_TRACE_ENABLED
        if (ExecutorContext::getExecutorContext()->m_siteId == 0) {
            const std::string& hexString = createQueryStatement->explainplan();
            assert(hexString.length() % 2 == 0);
            int bufferLength = (int)hexString.size() / 2 + 1;
            char* explanation = new char[bufferLength];
            boost::shared_array<char> memoryGuard(explanation);
            catalog::Catalog::hexDecodeString(hexString, explanation);
            cout << m_destTable->name() << " MaterializedViewHandler::setUpCreateQuery()\n" << explanation << endl;
        }
#endif
    }

    void MaterializedViewHandler::setUpMinMaxQueries(catalog::MaterializedViewHandlerInfo *mvHandlerInfo,
                                                 VoltDBEngine *engine) {
        m_minMaxExecutorVectors.clear();
        BOOST_FOREACH (LabeledStatement labeledStatement, mvHandlerInfo->fallbackQueryStmts()) {
            catalog::Statement *stmt = labeledStatement.second;
            boost::shared_ptr<ExecutorVector> execVec = ExecutorVector::fromCatalogStatement(engine, stmt);
            execVec->getRidOfSendExecutor();
            m_minMaxExecutorVectors.push_back(execVec);
        }
    }

    // If the source table(s) is not empty when the view is created, we need to execute the plan directly
    // to catch up with the existing data.
    void MaterializedViewHandler::catchUpWithExistingData() {
        if (! m_destTable->isPersistentTableEmpty()) {
            return;
        }
        bool hasNonEmptySourceTable = false;
        for (auto sourceTable : m_sourceTables) {
            if ( ! sourceTable->isPersistentTableEmpty()) {
                hasNonEmptySourceTable = true;
                break;
            }
        }
        if (! hasNonEmptySourceTable) {
            return;
        }
        ExecutorContext* ec = ExecutorContext::getExecutorContext();
        vector<AbstractExecutor*> executorList = m_createQueryExecutorVector->getExecutorList();
        Table *viewContent = ec->executeExecutors(executorList);
        TableIterator ti = viewContent->iterator();
        TableTuple tuple(viewContent->schema());
        while (ti.next(tuple)) {
            m_destTable->insertTuple(tuple);
        }
        ec->cleanupExecutorsForSubquery(executorList);
    }

    void MaterializedViewHandler::setUpBackedTuples() {
        m_existingTuple = TableTuple(m_destTable->schema());
        m_updatedTupleStorage.init(m_destTable->schema());
        m_updatedTuple = m_updatedTupleStorage.tuple();
    }

    bool MaterializedViewHandler::findExistingTuple(const TableTuple &deltaTuple) {
        // For the case where there is no grouping column, like SELECT COUNT(*) FROM T;
        // We directly return the only row in the view. See ENG-7872.
        if (m_groupByColumnCount == 0) {
            TableIterator iterator = m_destTable->iterator();
            iterator.next(m_existingTuple);
            // Please note that if there is no group by columns, the view shall always have one row.
            // This row will be initialized when the view is constructed. We have special code path for that. -yzhang
            assert( ! m_existingTuple.isNullTuple());
            return true;
        }

        IndexCursor indexCursor(m_index->getTupleSchema());
        // determine if the row exists (create the empty one if it doesn't)
        m_index->moveToKeyByTuple(&deltaTuple, indexCursor);
        m_existingTuple = m_index->nextValueAtKey(indexCursor);
        return ! m_existingTuple.isNullTuple();
    }

    void MaterializedViewHandler::mergeTupleForInsert(const TableTuple &deltaTuple) {
#ifdef VOLT_TRACE_ENABLED
        if (ExecutorContext::getExecutorContext()->m_siteId == 0)
            cout << m_destTable->m_name << " MaterializedViewHandler::mergeTupleForInsert()" << endl;
#endif
        // set up the group-by columns
        for (int colindex = 0; colindex < m_groupByColumnCount; colindex++) {
            // note that if the tuple is in the mv's target table,
            // tuple values should be pulled from the existing tuple in
            // that table. This works around a memory ownership issue
            // related to out-of-line strings.
            NValue value = m_existingTuple.getNValue(colindex);
            m_updatedTuple.setNValue(colindex, value);
        }
        // COUNT(*)
        NValue oldCount = m_existingTuple.getNValue(m_groupByColumnCount);
        NValue deltaCount = deltaTuple.getNValue(m_groupByColumnCount);
        m_updatedTuple.setNValue(m_groupByColumnCount, oldCount.op_add(deltaCount));
        // Aggregations
        int aggOffset = m_groupByColumnCount + 1;
        for (int aggIndex = 0; aggIndex < m_aggColumnCount; aggIndex++) {
            NValue existingValue = m_existingTuple.getNValue(aggOffset+aggIndex);
            NValue newValue = deltaTuple.getNValue(aggOffset+aggIndex);
            if (newValue.isNull()) {
                newValue = existingValue;
            }
            else {
                switch(m_aggTypes[aggIndex]) {
                    case EXPRESSION_TYPE_AGGREGATE_SUM:
                    case EXPRESSION_TYPE_AGGREGATE_COUNT:
                        if (!existingValue.isNull()) {
                            newValue = existingValue.op_add(newValue);
                        }
                        break;
                    case EXPRESSION_TYPE_AGGREGATE_MIN:
                        // ignore any new value that is not strictly an improvement
                        if (!existingValue.isNull() && newValue.compare(existingValue) >= 0) {
                            newValue = existingValue;
                        }
                        break;
                    case EXPRESSION_TYPE_AGGREGATE_MAX:
                        // ignore any new value that is not strictly an improvement
                        if (!existingValue.isNull() && newValue.compare(existingValue) <= 0) {
                            newValue = existingValue;
                        }
                        break;
                    default:
                        assert(false); // Should have been caught when the matview was loaded.
                        // no break
                }
            }
            m_updatedTuple.setNValue(aggOffset+aggIndex, newValue);
        }
    }

    void MaterializedViewHandler::handleTupleInsert(PersistentTable *sourceTable, bool fallible) {
        // Within the lifespan of this ScopedDeltaTableContext, the changed source table will enter delta table mode.
        ScopedDeltaTableContext dtContext(sourceTable);
        ExecutorContext* ec = ExecutorContext::getExecutorContext();
        vector<AbstractExecutor*> executorList = m_createQueryExecutorVector->getExecutorList();
        Table *delta = ec->executeExecutors(executorList);
#ifdef VOLT_TRACE_ENABLED
        if (ExecutorContext::getExecutorContext()->m_siteId == 0)
            cout << m_destTable->m_name << " MaterializedViewHandler::handleTupleInsert()" << endl;
#endif
        TableIterator ti = delta->iterator();
        TableTuple deltaTuple(delta->schema());
        while (ti.next(deltaTuple)) {
            bool found = findExistingTuple(deltaTuple);
#ifdef VOLT_TRACE_ENABLED
            if (ExecutorContext::getExecutorContext()->m_siteId == 0) {
                cout << "Delta tuple: \n" << deltaTuple.debug("delta result") << endl;
                if (found) {
                    cout << "Found in view:\n" << m_existingTuple.debug("existing tuple") << endl;
                }
                else {
                    cout << "Not found in view.\n";
                }
            }
#endif
            if (found) {
                mergeTupleForInsert(deltaTuple);
                // Shouldn't need to update group-key-only indexes such as the primary key
                // since their keys shouldn't ever change, but do update other indexes.
                m_destTable->updateTupleWithSpecificIndexes(m_existingTuple, m_updatedTuple,
                                                            m_updatableIndexList, fallible);
            }
            else {
                m_destTable->insertPersistentTuple(deltaTuple, fallible);
            }
        }
        ec->cleanupExecutorsForSubquery(executorList);
    }

    void MaterializedViewHandler::handleTupleDelete(PersistentTable *sourceTable, bool fallible) {
//         // Within the lifespan of this ScopedDeltaTableContext, the changed source table will enter delta table mode.
//         ScopedDeltaTableContext dtContext(sourceTable);
//         ExecutorContext* ec = ExecutorContext::getExecutorContext();
//         vector<AbstractExecutor*> executorList = m_createQueryExecutorVector->getExecutorList();
//         Table *delta = ec->executeExecutors(executorList);
// #ifdef VOLT_TRACE_ENABLED
//         if (ExecutorContext::getExecutorContext()->m_siteId == 0)
//             cout << m_destTable->m_name << " MaterializedViewHandler::handleTupleDelete()" << endl;
// #endif
//         TableIterator ti = delta->iterator();
//         TableTuple deltaTuple(delta->schema());
//         while (ti.next(deltaTuple)) {
//             bool found = findExistingTuple(deltaTuple);
// #ifdef VOLT_TRACE_ENABLED
//             if (ExecutorContext::getExecutorContext()->m_siteId == 0) {
//                 cout << "Delta tuple: \n" << deltaTuple.debug("delta result") << endl;
//                 if (found) {
//                     cout << "Found in view:\n" << m_existingTuple.debug("existing tuple") << endl;
//                 }
//                 else {
//                     cout << "Not found in view.\n";
//                 }
//             }
// #endif
//         }
    }

} // namespace voltdb