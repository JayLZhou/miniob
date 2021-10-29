/* Copyright (c) 2021 Xie Meiyi(xiemeiyi@hust.edu.cn) and OceanBase and/or its affiliates. All rights reserved.
miniob is licensed under Mulan PSL v2.
You can use this software according to the terms and conditions of the Mulan PSL v2.
You may obtain a copy of Mulan PSL v2 at:
         http://license.coscl.org.cn/MulanPSL2
THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND,
EITHER EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT,
MERCHANTABILITY OR FIT FOR A PARTICULAR PURPOSE.
See the Mulan PSL v2 for more details. */

//
// Created by Longda on 2021/4/13.
//

#include <memory>
#include <string>
#include <sstream>
#include <cmath>
#include <algorithm>

#include "execute_stage.h"

#include "common/io/io.h"
#include "common/log/log.h"
#include "common/seda/timer_stage.h"
#include "common/lang/string.h"
#include "session/session.h"
#include "event/storage_event.h"
#include "event/sql_event.h"
#include "event/session_event.h"
#include "event/execution_plan_event.h"
#include "sql/executor/execution_node.h"
#include "sql/executor/tuple.h"
#include "storage/common/table.h"
#include "storage/default/default_handler.h"
#include "storage/common/condition_filter.h"
#include "storage/trx/trx.h"

using namespace common;

RC create_selection_executor(Trx *trx, const Selects &selects, const char *db, const char *table_name,
                             SelectExeNode &select_node, SessionEvent *session_event);

void cross_join(std::vector<TupleSet> &tuple_sets, const Selects &selects, TupleSet &tuple_set);

void do_cross_join(std::vector<TupleSet> &tuple_sets, int index, std::vector<const Condition *> conditions,
                   TupleSet &tuple_set, Tuple &tuple);

//! Constructor
//! Constructor
ExecuteStage::ExecuteStage(const char *tag) : Stage(tag) {}

//! Destructor
ExecuteStage::~ExecuteStage() {}

//! Parse properties, instantiate a stage object
Stage *ExecuteStage::make_stage(const std::string &tag) {
    ExecuteStage *stage = new(std::nothrow) ExecuteStage(tag.c_str());
    if (stage == nullptr) {
        LOG_ERROR("new ExecuteStage failed");
        return nullptr;
    }
    stage->set_properties();
    return stage;
}

//! Set properties for this object set in stage specific properties
bool ExecuteStage::set_properties() {
    //  std::string stageNameStr(stageName);
    //  std::map<std::string, std::string> section = theGlobalProperties()->get(
    //    stageNameStr);
    //
    //  std::map<std::string, std::string>::iterator it;
    //
    //  std::string key;

    return true;
}

//! Initialize stage params and validate outputs
bool ExecuteStage::initialize() {
    LOG_TRACE("Enter");

    std::list<Stage *>::iterator stgp = next_stage_list_.begin();
    default_storage_stage_ = *(stgp++);
    mem_storage_stage_ = *(stgp++);

    LOG_TRACE("Exit");
    return true;
}

//! Cleanup after disconnection
void ExecuteStage::cleanup() {
    LOG_TRACE("Enter");

    LOG_TRACE("Exit");
}

void ExecuteStage::handle_event(StageEvent *event) {
    LOG_TRACE("Enter\n");

    handle_request(event);

    LOG_TRACE("Exit\n");
    return;
}

void ExecuteStage::callback_event(StageEvent *event, CallbackContext *context) {
    LOG_TRACE("Enter\n");

    // here finish read all data from disk or network, but do nothing here.
    ExecutionPlanEvent *exe_event = static_cast<ExecutionPlanEvent *>(event);
    SQLStageEvent *sql_event = exe_event->sql_event();
    sql_event->done_immediate();

    LOG_TRACE("Exit\n");
    return;
}

void ExecuteStage::handle_request(common::StageEvent *event) {
    ExecutionPlanEvent *exe_event = static_cast<ExecutionPlanEvent *>(event);
    SessionEvent *session_event = exe_event->sql_event()->session_event();
    Query *sql = exe_event->sqls();
    const char *current_db = session_event->get_client()->session->get_current_db().c_str();

    CompletionCallback *cb = new(std::nothrow) CompletionCallback(this, nullptr);
    if (cb == nullptr) {
        LOG_ERROR("Failed to new callback for ExecutionPlanEvent");
        exe_event->done_immediate();
        return;
    }
    exe_event->push_callback(cb);

    switch (sql->flag) {
        case SCF_SELECT: { // select
            do_select(current_db, sql, exe_event->sql_event()->session_event());
            exe_event->done_immediate();
        }
            break;

        case SCF_INSERT:
        case SCF_UPDATE:
        case SCF_DELETE:
        case SCF_CREATE_TABLE:
        case SCF_SHOW_TABLES:
        case SCF_DESC_TABLE:
        case SCF_DROP_TABLE:
        case SCF_CREATE_INDEX:
        case SCF_DROP_INDEX:
        case SCF_LOAD_DATA: {
            StorageEvent *storage_event = new(std::nothrow) StorageEvent(exe_event);
            if (storage_event == nullptr) {
                LOG_ERROR("Failed to new StorageEvent");
                event->done_immediate();
                return;
            }

            default_storage_stage_->handle_event(storage_event);
        }
            break;
        case SCF_SYNC: {
            RC rc = DefaultHandler::get_default().sync();
            session_event->set_response(strrc(rc));
            exe_event->done_immediate();
        }
            break;
        case SCF_BEGIN: {
            session_event->get_client()->session->set_trx_multi_operation_mode(true);
            session_event->set_response(strrc(RC::SUCCESS));
            exe_event->done_immediate();
        }
            break;
        case SCF_COMMIT: {
            Trx *trx = session_event->get_client()->session->current_trx();
            RC rc = trx->commit();
            session_event->get_client()->session->set_trx_multi_operation_mode(false);
            session_event->set_response(strrc(rc));
            exe_event->done_immediate();
        }
            break;
        case SCF_ROLLBACK: {
            Trx *trx = session_event->get_client()->session->current_trx();
            RC rc = trx->rollback();
            session_event->get_client()->session->set_trx_multi_operation_mode(false);
            session_event->set_response(strrc(rc));
            exe_event->done_immediate();
        }
            break;
        case SCF_HELP: {
            const char *response = "show tables;\n"
                                   "desc `table name`;\n"
                                   "create table `table name` (`column name` `column type`, ...);\n"
                                   "create index `index name` on `table` (`column`);\n"
                                   "insert into `table` values(`value1`,`value2`);\n"
                                   "update `table` set column=value [where `column`=`value`];\n"
                                   "delete from `table` [where `column`=`value`];\n"
                                   "select [ * | `columns` ] from `table`;\n";
            session_event->set_response(response);
            exe_event->done_immediate();
        }
            break;
        case SCF_EXIT: {
            // do nothing
            const char *response = "Unsupported\n";
            session_event->set_response(response);
            exe_event->done_immediate();
        }
            break;
        default: {
            exe_event->done_immediate();
            LOG_ERROR("Unsupported command=%d\n", sql->flag);
        }
    }
}

void end_trx_if_need(Session *session, Trx *trx, bool all_right) {
    if (!session->is_trx_multi_operation_mode()) {
        if (all_right) {
            trx->commit();
        } else {
            trx->rollback();
        }
    }
}


bool is_valid_aggre(char *attr, AggreType aggre_type) {  // number, float, *
    if (strcmp("*", attr) == 0) {
        if(aggre_type==COUNT)
            return true;
        else{
            return false;
        }
    }

    int i = 0;
    int length = strlen(attr);
    if (i >= length) return false;

    if (!('0' <= attr[i] && attr[i] <= '9') && attr[i] != '-' && attr[i] != '+') {
        return false;
    }

    for (; i < length; i++) {
        if (('0' <= attr[i] && attr[i] <= '9') || attr[i] == '.') {
            if (i == length - 1) {
                return true;
            }
            continue;
        } else {
            return false;
        }
    }
}

void parse_attr(char *attribute_name, AggreType aggre_type, char *attr_name) {
    if (aggre_type == NON) return;
    int j = 0;
    for (int i = 0; i < strlen(attribute_name); i++) {
        if (i == 0) {
            if (aggre_type == COUNT) {  // COUNT{
                i += 5;
            } else {
                i += 3;
            }
        }
        if (attribute_name[i] == '(' || attribute_name[i] == ')' || attribute_name[i] == ' ') {
            continue;
        }
        attr_name[j++] = attribute_name[i];
    }
    attr_name[j] = '\0';
}

RC ExecuteStage::do_aggregate(const Selects &selects, TupleSet &tuple_set, TupleSet &aggred_tupleset) {
    // schema
    TupleSchema tuple_schema;
    for (size_t i = 0; i < selects.attr_num; i++) {
        RelAttr attr = selects.attributes[selects.attr_num - 1 - i];  // todo: 0 relation
        int schema_index = 0;
        char parsed[100];
        parse_attr(attr.attribute_name, attr.aggre_type, parsed);
        if (attr.aggre_type != NON && is_valid_aggre(parsed, attr.aggre_type)) {  // count(1) find the first one attr
            schema_index = 0;
        } else {
            schema_index = tuple_set.get_schema().index_of_field(selects.relations[0], parsed);
        }
        if (schema_index < 0 || schema_index >= (int) tuple_set.get_schema().fields().size()) {
            continue;
        }
        AttrType attr_type = tuple_set.get_schema().field(schema_index).type();
        if (attr_type == INTS && attr.aggre_type == AVG) {
            attr_type = FLOATS;
        }
        tuple_schema.add(attr_type, selects.relations[0], attr.attribute_name);
    }
    aggred_tupleset.set_schema(tuple_schema);

    // tuple (only one tuple)
    Tuple aggred_tuple;
    for (size_t i = 0; i < selects.attr_num; i++) {
        RelAttr attr = selects.attributes[selects.attr_num - 1 - i];
        int index = 0;
        char parsed[100];
        parse_attr(attr.attribute_name, attr.aggre_type, parsed);
        if (attr.aggre_type != NON && is_valid_aggre(parsed, attr.aggre_type)) {
            index = 0;
        } else {
            index = tuple_set.get_schema().index_of_field(selects.relations[0], parsed);
        }
        if (attr.aggre_type == COUNT) {
            // null check here
            aggred_tuple.add(tuple_set.size());
        } else if (attr.aggre_type == MIN) {
            int min_index = 0;
            for (size_t j = 0; j < tuple_set.size(); j++) {
                if (tuple_set.get(min_index).get(index).compare(tuple_set.get(j).get(index)) > 0) {
                    min_index = j;
                }
            }
            int t1 = 0;
            bool flag = false;
            for (auto &temp1: tuple_set.tuples()) {
                int t2 = 0;
                for (auto &value: temp1.values()) {
                    if (t1 == min_index && t2 == index) {
                        if (tuple_set.get_schema().field(index).type() == FLOATS) {
                            FloatValue *floatvalue = dynamic_cast<FloatValue *>(value.get());
                            float fvalue = round(100 * (floatvalue->get_value())) / 100.0;
                            aggred_tuple.add(fvalue);
                        } else {
                            aggred_tuple.add(value);
                        }
                        flag = true;
                        break;
                    }
                    t2++;
                }
                if (flag) break;
                t1++;
            }
            // aggred_tuple.add(tuple_set.get(min_index).get(index));
        } else if (attr.aggre_type == MAX) {
            int max_index = 0;
            for (size_t j = 0; j < tuple_set.size(); j++) {
                if (tuple_set.get(max_index).get(index).compare(tuple_set.get(j).get(index)) < 0) {
                    max_index = j;
                }
            }
            int t1 = 0;
            bool flag = false;
            for (auto &temp1: tuple_set.tuples()) {
                int t2 = 0;
                for (auto &value: temp1.values()) {
                    if (t1 == max_index && t2 == index) {
                        if (tuple_set.get_schema().field(index).type() == FLOATS) {
                            FloatValue *floatvalue = dynamic_cast<FloatValue *>(value.get());
                            float fvalue = round(100 * (floatvalue->get_value())) / 100.0;
                            aggred_tuple.add(fvalue);
                        } else {
                            aggred_tuple.add(value);
                        }

                        flag = true;
                        break;
                    }
                    t2++;
                }
                if (flag) break;
                t1++;
            }
        } else if (attr.aggre_type == AVG) {
            AttrType type = tuple_set.get_schema().field(index).type();
            if (type != FLOATS && type != INTS) {
                return RC::GENERIC_ERROR;
            }
            if (type == FLOATS) {
                float sum = 0.0;
                for (auto &temp1: tuple_set.tuples()) {
                    int t2 = 0;
                    for (auto &value: temp1.values()) {
                        if (t2 == index) {
                            FloatValue *floatvalue = dynamic_cast<FloatValue *>(value.get());
                            sum += floatvalue->get_value();
                        }
                        t2++;
                    }
                }
                float avg = round(100 * sum / tuple_set.size()) / 100.0;
                aggred_tuple.add(avg);

            }
            if (type == INTS) {
                int sum = 0;
                for (auto &temp1: tuple_set.tuples()) {
                    int t2 = 0;
                    for (auto &value: temp1.values()) {
                        if (t2 == index) {
                            IntValue *intvalue = dynamic_cast<IntValue *>(value.get());
                            sum += intvalue->get_value();
                        }
                        t2++;
                    }
                }
                float avg = round(100 * (float) sum / tuple_set.size()) / 100.0;
                aggred_tuple.add(avg);
            }
        }

    }
    aggred_tupleset.add(std::move(aggred_tuple));
    return RC::SUCCESS;
}


// 这里没有对输入的某些信息做合法性校验，比如查询的列名、where条件中的列名等，没有做必要的合法性校验
// 需要补充上这一部分. 校验部分也可以放在resolve，不过跟execution放一起也没有关系
RC ExecuteStage::do_select(const char *db, Query *sql, SessionEvent *session_event) {

    RC rc = RC::SUCCESS;
    Session *session = session_event->get_client()->session;
    Trx *trx = session->current_trx();
    const Selects &selects = sql->sstr.selection;
    // 把所有的表和只跟这张表关联的condition都拿出来，生成最底层的select 执行节点
    std::vector<SelectExeNode *> select_nodes;
    for (size_t i = 0; i < selects.relation_num; i++) {
        const char *table_name = selects.relations[i];
        SelectExeNode *select_node = new SelectExeNode;
        rc = create_selection_executor(trx, selects, db, table_name, *select_node, session_event);
        if (rc != RC::SUCCESS) {
            delete select_node;
            for (SelectExeNode *&tmp_node: select_nodes) {
                delete tmp_node;
            }
            end_trx_if_need(session, trx, false);
            return rc;
        }
        select_nodes.push_back(select_node);
    }

    if (select_nodes.empty()) {
        LOG_ERROR("No table given");
        end_trx_if_need(session, trx, false);
        return RC::SQL_SYNTAX;
    }

    std::vector<TupleSet> tuple_sets;
    for (SelectExeNode *&node: select_nodes) {
        TupleSet tuple_set;
        rc = node->execute(tuple_set);
        if (rc != RC::SUCCESS) {
            for (SelectExeNode *&tmp_node: select_nodes) {
                delete tmp_node;
            }
            end_trx_if_need(session, trx, false);
            return rc;
        } else {
            // tuple_set 是一张表上得到的所有tuple查询结果
            RelAttr attr = selects.attributes[0];
            if (attr.aggre_type != NON) {
                TupleSet aggred_tupleset;
                do_aggregate(selects, tuple_set, aggred_tupleset);
                tuple_sets.push_back(std::move(aggred_tupleset));
            } else {
                tuple_sets.push_back(std::move(tuple_set));
            }
        }
    }

    std::stringstream ss;
    if (select_nodes.size() > 1) {
        TupleSet tuple_set;
        /*
        int i = 0;
        int j = tuple_sets.size() - 1;
        while (i < j) {
          tuple_set = std::move(tuple_sets[i]);
          tuple_sets[i] = std::move(tuple_sets[j]);
          tuple_sets[j] = std::move(tuple_set);
          i++, j--;
        }*/
        cross_join(tuple_sets, selects, tuple_set);
        tuple_set.print_with_tablename(ss);
        // 本次查询了多张表，需要做join操作
    } else {
        // 当前只查询一张表，直接返回结果即可
        tuple_sets.front().print(ss);
    }

    for (SelectExeNode *&tmp_node: select_nodes) {
        delete tmp_node;
    }
    session_event->set_response(ss.str());
    end_trx_if_need(session, trx, true);
    return rc;
}

bool match_table(const Selects &selects, const char *table_name_in_condition, const char *table_name_to_match) {
    if (table_name_in_condition != nullptr) {
        return 0 == strcmp(table_name_in_condition, table_name_to_match);
    }

    return selects.relation_num == 1;
}

static RC schema_add_field(Table *table, const char *field_name, TupleSchema &schema) {
    const FieldMeta *field_meta = table->table_meta().field(field_name);
    if (nullptr == field_meta) {
        LOG_WARN("No such field. %s.%s", table->name(), field_name);
        return RC::SCHEMA_FIELD_MISSING;
    }

    schema.add_if_not_exists(field_meta->type(), table->name(), field_meta->name());
    return RC::SUCCESS;
}

// 把所有的表和只跟这张表关联的condition都拿出来，生成最底层的select 执行节点
RC create_selection_executor(Trx *trx, const Selects &selects, const char *db,
                             const char *table_name, SelectExeNode &select_node, SessionEvent *session_event) {
    // 列出跟这张表关联的Attr
    TupleSchema schema;
    Table *table = DefaultHandler::get_default().find_table(db, table_name);
    char response[256];
    if (nullptr == table) {
        LOG_WARN("No such table [%s] in db [%s]", table_name, db);
        // snprintf(response, sizeof(response), "Table '%s' dosen't exist\n", table_name);
        snprintf(response, sizeof(response), "FAILURE\n");
        session_event->set_response(response);
        return RC::SCHEMA_TABLE_NOT_EXIST;
    }

    for (int i = selects.attr_num - 1; i >= 0; i--) {
        const RelAttr &attr = selects.attributes[i];
        if (nullptr == attr.relation_name || 0 == strcmp(table_name, attr.relation_name)) {

            char parsed[100];

            parse_attr(attr.attribute_name, attr.aggre_type, parsed); // if not aggre, will do nothing and return
            if (0 == strcmp("*", attr.attribute_name) || (attr.aggre_type != NON && is_valid_aggre(parsed, attr.aggre_type))) {

                // 列出这张表所有字段
                TupleSchema::from_table(table, schema);
                break; // 没有校验，给出* 之后，再写字段的错误
            } else {
                // 列出这张表相关字段

                RC rc = RC::SUCCESS;
                if (attr.aggre_type != NON) {
                    rc = schema_add_field(table, parsed, schema);
                } else {
                    rc = schema_add_field(table, attr.attribute_name, schema);
                }

                if (rc != RC::SUCCESS) {
                    if (rc == RC::SCHEMA_FIELD_MISSING) {
                        //  snprintf(response, sizeof(response), "Unknown column '%s' in 'field list'\n", attr.attribute_name);
                        snprintf(response, sizeof(response), "FAILURE\n");
                        session_event->set_response(response);
                    }
                    return rc;
                }
            }
        }
    }

    // 找出仅与此表相关的过滤条件, 或者都是值的过滤条件
    std::vector<DefaultConditionFilter *> condition_filters;
    for (size_t i = 0; i < selects.condition_num; i++) {
        const Condition &condition = selects.conditions[i];
        if ((condition.left_is_attr == 0 && condition.right_is_attr == 0) || // 两边都是值
            (condition.left_is_attr == 1 && condition.right_is_attr == 0 &&
             match_table(selects, condition.left_attr.relation_name, table_name)) ||  // 左边是属性右边是值
            (condition.left_is_attr == 0 && condition.right_is_attr == 1 &&
             match_table(selects, condition.right_attr.relation_name, table_name)) ||  // 左边是值，右边是属性名
            (condition.left_is_attr == 1 && condition.right_is_attr == 1 &&
             match_table(selects, condition.left_attr.relation_name, table_name) &&
             match_table(selects, condition.right_attr.relation_name, table_name)) // 左右都是属性名，并且表名都符合
                ) {
            DefaultConditionFilter *condition_filter = new DefaultConditionFilter();
            RC rc = condition_filter->init(*table, condition);
            if (rc != RC::SUCCESS) {
                if (rc == RC::SCHEMA_FIELD_MISSING) {
                    // snprintf(response, sizeof(response), "Unknown column in 'clause'\n");
                    snprintf(response, sizeof(response), "FAILURE\n");
                    session_event->set_response(response);
                }
                delete condition_filter;
                for (DefaultConditionFilter *&filter: condition_filters) {
                    delete filter;
                }
                return rc;
            }
            condition_filters.push_back(condition_filter);
        }
    }

    return select_node.init(trx, table, std::move(schema), std::move(condition_filters));
}

void cross_join(std::vector<TupleSet> &tuple_sets, const Selects &selects, TupleSet &tuple_set) {
    // conditions for cross join
    std::vector<const Condition *> conditions;
    for (size_t i = 0; i < selects.condition_num; i++) {
        const Condition &condition = selects.conditions[i];
        if (condition.left_is_attr == 1 && condition.right_is_attr == 1 &&
            0 != strcmp(condition.left_attr.relation_name, condition.right_attr.relation_name)) {
            conditions.push_back(&condition);
        }
    }

    // schema for new tuple_set
    TupleSchema tuple_schema;
    for (int i = selects.attr_num - 1; i >= 0; i--) {
        const RelAttr &attr = selects.attributes[i];
        if (attr.relation_name == nullptr && 0 == strcmp("*", attr.attribute_name)) {
            for (int j = tuple_sets.size() - 1; j >= 0; j--) {
                tuple_schema.append(tuple_sets[j].schema());
            }
            break;
        } else {
            for (auto &tuple_set1: tuple_sets) {
                if (0 == strcmp(tuple_set1.get_schema().fields()[0].table_name(), attr.relation_name)) {
                    if (0 == strcmp("*", attr.attribute_name)) {
                        tuple_schema.append(tuple_set1.get_schema());
                        break;
                    } else {
                        const TupleSchema &schema = tuple_set1.get_schema();
                        for (auto &tuple_field: schema.fields()) {
                            if (0 == strcmp(tuple_field.field_name(), attr.attribute_name)) {
                                tuple_schema.add(tuple_field.type(), tuple_field.table_name(),
                                                 tuple_field.field_name());
                                break;
                            }
                        }
                    }
                }
            }
            // TODO: need check
        }
    }

    tuple_set.set_schema(tuple_schema);

    Tuple tuple(tuple_schema.fields().size());
    do_cross_join(tuple_sets, 0, conditions, tuple_set, tuple);
}

void do_cross_join(std::vector<TupleSet> &tuple_sets, int index, std::vector<const Condition *> conditions,
                   TupleSet &tuple_set, Tuple &tuple) {
    int size = tuple_sets.size();
    if (index == size) {
        for (auto &condition: conditions) {
            char *left_table = condition->left_attr.relation_name;
            char *left_attr = condition->left_attr.attribute_name;
            char *right_table = condition->right_attr.relation_name;
            char *right_attr = condition->right_attr.attribute_name;
            int i = tuple_set.get_schema().index_of_field(left_table, left_attr);
            int j = tuple_set.get_schema().index_of_field(right_table, right_attr);

            int result = tuple.get(i).compare(tuple.get(j));
            if ((result == 0 && (condition->comp == CompOp::EQUAL_TO || condition->comp == CompOp::GREAT_EQUAL ||
                                 condition->comp == CompOp::LESS_EQUAL)) ||
                (result == 1 && (condition->comp == CompOp::GREAT_THAN || condition->comp == CompOp::GREAT_EQUAL)) ||
                (result == -1 && (condition->comp == CompOp::LESS_THAN || condition->comp == CompOp::LESS_EQUAL))) {
                continue;
            }
            return;
        }
        Tuple new_tuple;
        for (auto &value: tuple.values()) {
            new_tuple.add(value);
        }
        tuple_set.add(std::move(new_tuple));
        return;
    }

    const TupleSet &tuple_set1 = tuple_sets[index];
    const std::vector<TupleField> &fields = tuple_set1.get_schema().fields();

    for (auto &tuple1: tuple_set1.tuples()) {
        for (int i = 0; i < fields.size(); i++) {
            int j = tuple_set.get_schema().index_of_field(fields[i].table_name(), fields[i].field_name());
            tuple.set(j, const_cast<std::shared_ptr<TupleValue> &>(tuple1.get_pointer(i)));
        }
        do_cross_join(tuple_sets, index + 1, conditions, tuple_set, tuple);
    }
}

