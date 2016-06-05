#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <algorithm>
#include <map>
#include <set>
#include <string>
#include <vector>
#include <iostream>
#include <sstream>
#include <sys/stat.h>

#include "FLOPPYParser.h"
#include "heap.h"
#include "libTinyFS.h"
#include "relAlg.h"
#include "FLOPPY_statements/statements.h"
#include "TupleIterator.h"
#include "bufferManager.h"

#define BUF_BLOCKS 500
#define CACHE_BLOCKS 500

using namespace std;

// TODO clean this up, separate into multiple files

enum QueryNodeType {TABLE, SELECT, PROJECT, DUPLICATE, PRODUCT, JOIN, GROUP, SORT, LIMIT};

struct QueryPlan {
   QueryNodeType type;
   int impl;
   union {
      char *strVal;
      int intVal;
      FLOPPYNode *cond;
      vector<char *> *attList;

      struct {
         vector<char *> *groupBy;
         vector<Aggregate> *aggregates;
      } grouping;
   };
   QueryPlan *left, *right; // subtrees, use only left if unary operation
};


Buffer *buffer;

vector<Aggregate> *getAggsFromCondition(FLOPPYNode *node) {
   vector<Aggregate> *aggs = new vector<Aggregate>;

   if (node->_type == ConditionNode || node->_type == ExpressionNode) {
      vector<Aggregate> *temp = getAggsFromCondition(node->node.left);
      aggs->insert(aggs->end(), temp->begin(), temp->end());
      if (node->node.right) {
         temp = getAggsFromCondition(node->node.right);
         aggs->insert(aggs->end(), temp->begin(), temp->end());
      }
   }
   else if (node->_type == AggregateNode) {
      aggs->push_back(Aggregate(node->aggregate.op, node->aggregate.value->sVal));
   }

   return aggs;
}

QueryPlan *makeQueryPlan(FLOPPYSelectStatement *stm) {
   QueryPlan *plan = NULL, *temp;

   // Process FROM clause
   for (auto tableSpec = stm->tableSpecs->begin();
    tableSpec != stm->tableSpecs->end(); tableSpec++) {
      bool first = plan == NULL;
      if (!first) {
         temp = new QueryPlan;
         temp->type = PRODUCT;
         temp->left = plan;
      }

      plan = new QueryPlan;
      plan->type = TABLE;
      plan->strVal = (*tableSpec)->tableName;

      if (!first) {
         temp->right = plan;
         plan = temp;
      }
   }

   // Process WHERE clause
   if (stm->whereCondition) {
      temp = plan;
      plan = new QueryPlan;
      plan->type = SELECT;
      plan->cond = stm->whereCondition;
      plan->left = temp;
   }

   // Process GROUP BY and HAVING clauses
   if (stm->groupBy) {
      temp = plan;
      plan = new QueryPlan;
      plan->type = GROUP;
      plan->grouping.groupBy = stm->groupBy->groupByAttributes;
      plan->grouping.aggregates = stm->groupBy->havingCondition
                                  ? getAggsFromCondition(stm->groupBy->havingCondition)
                                  : new vector<Aggregate>;
      for (auto iter = stm->selectItems->begin(); iter != stm->selectItems->end(); iter++) {
         if ((*iter)->_type == AggregateType) {
            plan->grouping.aggregates->push_back(
                    Aggregate((*iter)->aggregate.op, (*iter)->aggregate.value->sVal));
         }
      }
      plan->left = temp;

      if (stm->groupBy->havingCondition) {
         temp = plan;
         plan = new QueryPlan;
         plan->type = SELECT;
         plan->cond = stm->groupBy->havingCondition;
         plan->left = temp;
      }
   }
   else { // handle case where select clause has aggregates but there is no group by
      vector<Aggregate> *aggs = new vector<Aggregate>;
      for (auto iter = stm->selectItems->begin(); iter != stm->selectItems->end(); iter++) {
         if ((*iter)->_type == AggregateType) {
            aggs->push_back(Aggregate((*iter)->aggregate.op, (*iter)->aggregate.value->sVal));
         }
      }

      if (!aggs->empty()) {
         temp = plan;
         plan = new QueryPlan;
         plan->type = GROUP;
         plan->grouping.groupBy = NULL;
         plan->grouping.aggregates = aggs;
         plan->left = temp;
      }
   }

   // Process ORDER BY clause
   if (stm->orderBys) {
      temp = plan;
      plan = new QueryPlan;
      plan->type = SORT;
      plan->attList = stm->orderBys;
      plan->left = temp;
   }
   
   // Process SELECT clause
   // if select *, don't do a projection
   if (stm->selectItems->at(0)->_type != StarType) {
      temp = plan;
      plan = new QueryPlan;
      plan->type = PROJECT;
      plan->attList = new vector<char *>;
      for (auto iter = stm->selectItems->begin(); iter != stm->selectItems->end(); iter++) {
         string str;
         switch ((*iter)->_type) {
            case AttributeType:
               str = (*iter)->attribute;
               break;
            case TableAttributeType:
               str = string((*iter)->tableAttribute.tableName) + "." +
                       (*iter)->tableAttribute.attribute;
               break;
            case AggregateType:
               switch ((*iter)->aggregate.op) {
                  case CountAggregate:
                     str = string("COUNT(") + (*iter)->aggregate.value->sVal + ")";
                       break;
                  case CountStarAggregate:
                     str = "COUNT(*)";
                       break;
                  case AverageAggregate:
                     str = string("AVG(") + (*iter)->aggregate.value->sVal + ")";
                       break;
                  case MinAggregate:
                     str = string("MIN(") + (*iter)->aggregate.value->sVal + ")";
                       break;
                  case MaxAggregate:
                     str = string("MAX(") + (*iter)->aggregate.value->sVal + ")";
                       break;
                  case SumAggregate:
                     str = string("SUM(") + (*iter)->aggregate.value->sVal + ")";
               }
         }
         plan->attList->push_back((char *)str.c_str());
      }
      plan->left = temp;
   }

   if (stm->distinct) {
      temp = plan;
      plan = new QueryPlan;
      plan->type = DUPLICATE;
      plan->left = temp;
   }

   // Process LIMIT clause
   if (stm->limit >= 0) {
      temp = plan;
      plan = new QueryPlan;
      plan->type = LIMIT;
      plan->intVal = stm->limit;
      plan->left = temp;
   }

   return plan;
}

bool tableExists(char *tableName) {
   return access(tableName, F_OK) == 0;
}

bool tableAttributeExists(char *attr, string tableName) {
   int fd = getFd((char *)tableName.c_str());

   RecordDesc desc;
   heapHeaderGetRecordDesc(buffer, fd, &desc);

   for (int i = 0; i < desc.numFields; i++) {
      if (!strcmp(attr, desc.fields[i].name))
         return true;
   }

   return false;
}

bool attributeExists(char *attr, set<string> tableNames) {
   for (auto iter = tableNames.begin(); iter != tableNames.end(); iter++) {
      if (tableAttributeExists(attr, *iter))
         return true;
   }
   return false;
}

bool validateFromClause(vector<FLOPPYTableSpec *> *tableSpecs,
 map<string, string> &tableAliases, set<string> &tableNames, string *errMsg) {
   for (auto iter = tableSpecs->begin(); iter != tableSpecs->end(); iter++) {
      FLOPPYTableSpec *tableSpec = *iter;

      if (!tableExists(tableSpec->tableName)) {
         *errMsg = "Table does not exist: " + string(tableSpec->tableName);
         return false;
      }

      if (tableSpec->alias) {
         if (tableAliases.count(tableSpec->alias) || tableNames.count(tableSpec->alias)) {
            *errMsg = "Duplicate alias name in FROM clause: " + string(tableSpec->alias);
            return false;
         }
         tableAliases[tableSpec->alias] = tableSpec->tableName;
      }
      else {
         if (tableNames.count(tableSpec->tableName)) {
            *errMsg = "Duplicate table name in FROM clause: " + string(tableSpec->tableName);
            return false;
         }
      }
      tableNames.insert(tableSpec->tableName);
   }
   return true;
}

// Used for WHERE and HAVING clauses, check that all referenced attributes exist
// TODO type check stuff ???
bool validateCondition(FLOPPYNode *node, set<string> &tableNames,
 string *errMsg, bool aggAllowed) {
   if (node->_type == ValueNode) {
      if (node->value->type() == AttributeValue) {
         bool good = attributeExists(node->value->sVal, tableNames);
         if (!good)
            *errMsg = "Attribute does not exist: " + string(node->value->sVal);
         return good;
      }
      return true;
   }
   else if (node->_type == AggregateNode) {
      if (aggAllowed) {
         bool good = !node->aggregate.value->sVal ||
                 attributeExists(node->aggregate.value->sVal, tableNames);
         if (!good)
            *errMsg = "Attribute does not exist: " + string(node->aggregate.value->sVal);
         return good;
      }
      else {
         *errMsg = "Aggregations not allowed in WHERE clause.";
         return false;
      }
   }

   // recursive call on children of tree
   return validateCondition(node->node.left, tableNames, errMsg, aggAllowed) &&
    (!node->node.right || validateCondition(node->node.right, tableNames, errMsg, aggAllowed));
}

// check that all attributes exist
bool validateAttList(vector<char *> *attList, set<string> &tableNames,
 string *errMsg) {
   for (auto iter = attList->begin(); iter != attList->end(); iter++) {
      if (!attributeExists(*iter, tableNames)) {
         *errMsg = "Attribute does not exist: " + string(*iter);
         return false;
      }
   }
   return true;
}

bool validateSelectClause(FLOPPYSelectStatement *stm,
 map<string, string> tableAliases, set<string> tableNames, string *errMsg) {
   for (auto iter = stm->selectItems->begin(); iter != stm->selectItems->end();
    iter++) {
      FLOPPYSelectItem *item = *iter;

      if (item->_type == AttributeType) {
         vector<char *> *group = stm->groupBy->groupByAttributes;
         if (stm->groupBy && find(group->begin(), group->end(), item->attribute) == group->end()) {
            *errMsg = "Attribute not included in GROUP BY: " + string(item->attribute);
            return false;
         }

         if (!attributeExists(item->attribute, tableNames)) {
            *errMsg = "Attribute does not exist: " + string(item->attribute);
            return false;
         }
      }
      else if (item->_type == TableAttributeType) {
         // replace table aliases with actual table names
         if (tableAliases.count(item->tableAttribute.tableName))
            item->tableAttribute.tableName = (char *)tableAliases[item->tableAttribute.tableName].c_str();

         if (!tableNames.count(item->tableAttribute.tableName)) {
            *errMsg = "Table not included in FROM clause: " + string(item->tableAttribute.tableName);
            return false;
         }

         vector<char *> *group = stm->groupBy->groupByAttributes;
         if (stm->groupBy &&
          find(group->begin(), group->end(), item->tableAttribute.attribute) == group->end()) {
            *errMsg = "Attribute not included in GROUP BY: " + string(item->tableAttribute.attribute);
            return false;
         }

         if (!tableAttributeExists(item->tableAttribute.attribute,
          item->tableAttribute.tableName)) {
            *errMsg = "Attribute does not exist: " + string(item->tableAttribute.attribute);
            return false;
         }
      }
      else if (item->_type == AggregateType) {
         if (item->aggregate.value->sVal && !attributeExists(item->aggregate.value->sVal, tableNames)) {
            *errMsg = "Attribute does not exist: " + string(item->aggregate.value->sVal);
            return false;
         }
      }
   }

   return true;
}

bool validateSemantics(FLOPPYSelectStatement *stm, string *errMsg) {
   map<string, string> tableAliases; // map from alias to actual table name
   set<string> tableNames;

   // Validate FROM clause
   if (!validateFromClause(stm->tableSpecs, tableAliases, tableNames, errMsg))
      return false;

   // Validate WHERE clause if it exists
   if (stm->whereCondition &&
    !validateCondition(stm->whereCondition, tableNames, errMsg, false))
      return false;

   // Validate GROUP BY and HAVING clauses if they exist
   FLOPPYGroupBy *group = stm->groupBy;
   if (group) {
      if (!validateAttList(group->groupByAttributes, tableNames, errMsg))
         return false;
      if (group->havingCondition &&
       !validateCondition(group->havingCondition, tableNames, errMsg, true))
         return false;
   }

   // Validate ORDER BY clause
   if (stm->orderBys &&
    !validateAttList(stm->orderBys, tableNames, errMsg))
      return false;

   // Validate SELECT clause
   return validateSelectClause(stm, tableAliases, tableNames, errMsg);
}

void optimizeLogicalPlan(QueryPlan *plan) {
   // TODO
   // detect join conditions and change PRODUCT nodes into JOINs whenever possible
   // push other selections inside of joins when possible
}

// TODO doing this requires having B(R) and T(R) stored in the file headers
void makePhysicalPlan(QueryPlan *plan) {
   // go through plan and set the impl field of each non-leaf node
}

// this returns the fd of the file in which the result is stored
int executeQueryPlan(QueryPlan *plan) {
   int in, in2 = -1, out;

   switch (plan->type) {
      case TABLE: printf("executeQueryPlan: TABLE\n");
         return getFd(plan->strVal);
      case SELECT: printf("executeQueryPlan: SELECT\n");
         in = executeQueryPlan(plan->left);
         // TODO actually deal with index stuff
         //if (plan->impl == 0) {
            selectScan(in, plan->cond, &out);
         //}
         //else {
         //   selectIndex(in, plan->cond, index, &out);
         //}
         break;
      case PROJECT: printf("executeQueryPlan: PROJECT\n");
         in = executeQueryPlan(plan->left);
         project(in, plan->attList, &out);
         break;
      case DUPLICATE: printf("executeQueryPlan: DUPLICATE\n");
         in = executeQueryPlan(plan->left);
         duplicateElimination(in, &out);
         break;
      case PRODUCT: printf("executeQueryPlan: PRODUCT\n");
         in = executeQueryPlan(plan->left);
         in2 = executeQueryPlan(plan->right);
         product(in, in2, &out);
         break;
      case JOIN: printf("executeQueryPlan: JOIN\n");
         in = executeQueryPlan(plan->left);
         in2 = executeQueryPlan(plan->right);
         if (plan->impl == 0)
            joinOnePass(in, in2, plan->cond, &out);
         else if (plan->impl == 1)
            joinMultiPass(in, in2, plan->cond, &out);
         else
            joinNestedLoops(in, in2, plan->cond, &out);
         break;
      case GROUP: printf("executeQueryPlan: GROUP\n");
         in = executeQueryPlan(plan->left);
         if (plan->impl == 0)
            groupOnePass(in, plan->grouping.groupBy, plan->grouping.aggregates, &out);
         else
            groupMultiPass(in, plan->grouping.groupBy, plan->grouping.aggregates, &out);
         break;
      case SORT: printf("executeQueryPlan: SORT\n");
         in = executeQueryPlan(plan->left);
         sortTable(in, plan->attList, &out);
         break;
      case LIMIT: printf("executeQueryPlan: LIMIT\n");
         in = executeQueryPlan(plan->left);
         limitTable(in, plan->intVal, &out);
   }

   // delete temp tables
   if (plan->left->type != TABLE)
      tfs_deleteFile(in);
   if (in2 >= 0 && plan->right->type != TABLE)
      tfs_deleteFile(in2);

   return out;
}

void printTable(int fd) {
   RecordDesc recordDesc;

   heapHeaderGetRecordDesc(buffer, fd, &recordDesc);

   for (int i = 0; i < recordDesc.numFields; i++) {
      if (i > 0)
         printf(", ");
      printf("%s", recordDesc.fields[i].name);
   }
   printf("\n");

   TupleIterator iter(fd);
   for (Record *record = iter.next(); record; record = iter.next()) {
      for (int i = 0; i < recordDesc.numFields; i++) {
         if (i > 0)
            printf(", ");

         Field field = recordDesc.fields[i];

         if (field.type == VARCHAR)
            printf("'%s'", record->fields[field.name].sVal.c_str());
         else if (field.type == INT)
            printf("%d", record->fields[field.name].iVal);
         else if (field.type == BOOLEAN)
            printf("%d", record->fields[field.name].bVal);
         else
            printf("%f", record->fields[field.name].fVal);
      }
      printf("\n");
   }
}

void selectStatement(FLOPPYSelectStatement *stm) {
   string errMsg;

   if (validateSemantics(stm, &errMsg)) {
      QueryPlan *plan = makeQueryPlan(stm);
      optimizeLogicalPlan(plan);
      makePhysicalPlan(plan);
      int fd = executeQueryPlan(plan);
      printTable(fd);
      if (plan->type != TABLE) {// delete temp file
         printf("deleting file %d\n", fd);
         tfs_deleteFile(fd);
      }
   }
   else {
      printf("Error running query: %s\n", errMsg.c_str());
   }
}

// TODO add .dat extension to table files, and I guess .ind to index files
void createTableStatement(FLOPPYCreateTableStatement *stm) {
   if (access(stm->tableName.c_str(), F_OK) == 0) {
      struct stat st;
      stat(stm->tableName.c_str(), &st);
      if (st.st_size > 0) {
         printf("Table %s already exists.\n", stm->tableName.c_str());
         return;
      }
   }

   RecordDesc recordDesc;

   recordDesc.numFields = (int)stm->columns->size();
   for (int i = 0; i < recordDesc.numFields; i++) {
      FLOPPYCreateColumn *col = stm->columns->at(i);
      strcpy(recordDesc.fields[i].name, col->name);
      recordDesc.fields[i].type = col->type;
      switch (col->type) {
         case INT: recordDesc.fields[i].size = 4;
            break;
         case FLOAT: recordDesc.fields[i].size = 8;
            break;
         case DATETIME: recordDesc.fields[i].size = 8;
            break;
         case VARCHAR: recordDesc.fields[i].size = col->size + 1;
            break;
         case BOOLEAN: recordDesc.fields[i].size = 4;
      }
   }

   createHeapFile(buffer, (char *)stm->tableName.c_str(), recordDesc, stm->flags->volatileFlag);

   // TODO figure out persistent vs volatile files. For now, assume everything is persistent.
   // will need to put an isVolatile flag in the heap file header.

   // TODO create indexes for primary and foreign keys
}

void dropTableStatement(FLOPPYDropTableStatement *stm) {
   if (access(stm->table, F_OK) == -1) {
      printf("File named %s does not exist.\n", stm->table);
      return;
   }

   // This works for both persistent and volatile files.
   deleteHeapFile(buffer, stm->table);
}

void createIndexStatement(FLOPPYCreateIndexStatement *stm) {

}

void dropIndexStatement(FLOPPYDropIndexStatement *stm) {

}

void insertStatement(FLOPPYInsertStatement *stm) {
   DiskAddress temp;
   int recordSize;
   RecordDesc recordDesc;

   if (!tableExists(stm->name)) {
      printf("Table does not exist: %s\n", stm->name);
      return;
   }

   int fd = getFd(stm->name);
   heapHeaderGetRecordSize(buffer, fd, &recordSize);
   heapHeaderGetRecordDesc(buffer, fd, &recordDesc);

   if (stm->values->size() != recordDesc.numFields) {
      printf("Insert statement has %d values, expected %d.\n",
             stm->values->size(), recordDesc.numFields);
      return;
   }

   char *record = new char[recordSize];

   int byte = 0;
   for (int i = 0; i < stm->values->size(); i++) {
      FLOPPYValue *value = stm->values->at(i);

      if (recordDesc.fields[i].type == VARCHAR) {
         memset(&record[byte], 0, recordDesc.fields[i].size);
         strncpy(&record[byte], value->sVal, recordDesc.fields[i].size);
         byte += recordDesc.fields[i].size;
      }
      else if (recordDesc.fields[i].type == INT) {
         int remainder = byte % 4;
         if (remainder)
            byte += 4 - remainder;
         int val = (int)value->iVal;
         memcpy(&record[byte], &val, sizeof(int));
         byte += 4;
      }
      else if (recordDesc.fields[i].type == BOOLEAN) {
         int remainder = byte % 4;
         if (remainder)
            byte += 4 - remainder;
         int val = (int)value->bVal;
         memcpy(&record[byte], &val, sizeof(int));
         byte += 4;
      }
      else if (recordDesc.fields[i].type == FLOAT || recordDesc.fields[i].type == DATETIME) {
         int remainder = byte % 8;
         if (remainder)
            byte += 8 - remainder;
         double val = value->type() == IntValue ? value->iVal : value->fVal;
         memcpy(&record[byte], &val, sizeof(double));
         byte += 8;
      }
   }

   insertRecord(buffer, stm->name, record, &temp);
}

void deleteStatement(FLOPPYDeleteStatement *stm) {
   int fd = getFd(stm->name);
   TupleIterator iter(fd);

   for (Record *record = iter.next(); record; record = iter.next()) {
      if (checkCondition(record, stm->where)) {
         deleteRecord(buffer, record->page, record->ndx);
      }
   }
}

void updateStatement(FLOPPYUpdateStatement *stm) {
   int fd = getFd(stm->tableName);
   TupleIterator iter(fd);

   for (Record *record = iter.next(); record; record = iter.next()) {
      if (checkCondition(record, stm->whereExpression)) {
         RecordField field = evalExpr(record, stm->attributeExpression);
         if (record->fields[stm->attributeName].type == FLOAT &&
               field.type == INT) {
            field.type = FLOAT;
            field.fVal = field.iVal;
         }
         record->fields[stm->attributeName] = field;
         updateRecord(buffer, record->page, record->ndx, record->getBytes(record->recordDesc));
      }
   }
}

void runStatement(char *query) {
   FLOPPYOutput *result = FLOPPYParser::parseFLOPPYString(query);

   if (result->isValid) {
      FLOPPYStatement *stm = result->statement;
      switch (stm->type()) {
         case CreateTableStatement:
            createTableStatement((FLOPPYCreateTableStatement *) stm);
            break;
         case DropTableStatement:
            dropTableStatement((FLOPPYDropTableStatement *) stm);
            break;
         case CreateIndexStatement:
            createIndexStatement((FLOPPYCreateIndexStatement *) stm);
            break;
         case DropIndexStatement:
            dropIndexStatement((FLOPPYDropIndexStatement *) stm);
            break;
         case InsertStatement:
            insertStatement((FLOPPYInsertStatement *) stm);
            break;
         case DeleteStatement:
            deleteStatement((FLOPPYDeleteStatement *) stm);
            break;
         case UpdateStatement:
            updateStatement((FLOPPYUpdateStatement *) stm);
            break;
         case SelectStatement:
            selectStatement((FLOPPYSelectStatement *) stm);
            break;
         default:
            printf("what\n");
      }
   }
   else {
      printf("Failed to parse FLOPPY-SQL statement.\n");
   }
}

// Stuff that seems to work:
// - create table (volatile isn't handled yet though)
// - drop table // TODO stuff might not be getting removed from buffer?
// - insert
// - delete
// - update
// - single-table select with no aggregates
// Then get volatile files to work. Check that I can insert into a table and select those tuples back
// out without the disk file getting bigger?

int main() {
   buffer = (Buffer *)malloc(sizeof(Buffer));

   commence((char *)"db.dsk", buffer, BUF_BLOCKS, CACHE_BLOCKS);

   char query[2048];

   while (cin.good()) {
      cin.getline(query, 2048);

      runStatement(query);
   }

   squash(buffer);

   return 0;
}