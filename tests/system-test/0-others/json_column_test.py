# -*- coding: utf-8 -*-

import time
from util.log import *
from util.sql import *
from util.cases import *
from util.dnodes import *


class TDTestCase:
    clientCfgDict = {'debugFlag': 135}
    updatecfgDict = {'debugFlag': 135, 'clientCfg':clientCfgDict}

    def init(self, conn, logSql, replicaVar=1):
        self.replicaVar = int(replicaVar)
        tdLog.debug(f"start to excute {__file__}")
        tdSql.init(conn.cursor(), True)
        self.ttl = 5
        self.dbname = "test"
        self.jsonTemplate = '''{
            "k1": "string",
            "k2": "long",
            "k3": ["double"],
            "k4": "long",
            "k5": {
                "k6": "boolean",
                "k7": "double"
                },
            "k8": "string"
        }'''
        self.jsonTemplate1 = '''{
            "k1": "string",
            "k2": "long"
        }'''

    # def check_create_normal_table_result(self):
    #     createSql = "create table %s.t1(ts timestamp, c1 int, c2 json template '%s')" % (self.dbname, self.jsonTemplate)
    #     print(createSql)
    #     tdSql.execute(createSql)
    #     tdSql.query(f'desc t1')
    #     tdSql.checkRows(3)
    #     tdSql.checkData(0, 7, '')
    #     tdSql.checkData(1, 7, '')
    #     tdSql.checkData(2, 7, '1:{"k1":"string","k2":"long","k3":["double"],"k4":"long","k5":{"k6":"boolean","k7":"double"},"k8":"string"}')
    #     tdSql.execute(''' insert into t1(ts,c2) values(now,'{"Cmd":2,"Encrypt":1,"Vin":"1G1BL52P7TR115520","Data":{"Infos":[{"Motors":[{"CtrlTemp":125,"DCBusCurrent":31203,"InputVoltage":30012,"MotorTemp":125,"No":1,"Rotating":30000,"Status":1,"Torque":25000},{"CtrlTem
    # p":125,"DCBusCurrent":30200,"InputVoltage":32000,"MotorTemp":145,"No":2,"Rotating":30200,"Status":1,"Torque":25300}],"Number":2,"Type":"DriveMotor"}],"Time":{"Day":1,"Hour":2,"Minute":59,"Month":1,"Second":0,"Year":16}}}') ''')
    def check_create_normal_table_error(self):
        tdSql.error("create table t0(ts timestamp, c1 int, c2 json)")
        tdSql.error("create table t0(ts timestamp, c1 int, c2 json template)")
        tdSql.error("create table t0(ts timestamp, c1 int, c2 json template '')")
        tdSql.error("create table t0(ts timestamp, c1 int, c2 json template ' ')")
        tdSql.error("create table t0(ts timestamp, c1 int, c2 json template 'hello')")
        tdSql.error("create table t0(ts timestamp, c1 int, c2 json template '\"hello\"')")
        tdSql.error("create table t0(ts timestamp, c1 int, c2 json template 'null')")
        tdSql.error("create table t0(ts timestamp, c1 int, c2 json template 'true')")
        tdSql.error("create table t0(ts timestamp, c1 int, c2 json template true)")
        tdSql.error("create table t0(ts timestamp, c1 int, c2 json template '[]')")
        tdSql.error("create table t0(ts timestamp, c1 int, c2 json template 33)")
        tdSql.error("create table t0(ts timestamp, c1 int, c2 json template null)")
        tdSql.error("create table t0(ts timestamp, c1 int, c2 json template {})")
        tdSql.error("create table t0(ts timestamp, c1 int, c2 json template '{}')")
        tdSql.error('''create table t0(ts timestamp, c1 int, c2 int template '{"k1":"string"}')''')

    def check_create_normal_table_ok(self):
        createSql = "create table %s.t1(ts timestamp, c1 int, c2 json template '%s',c3 json template '%s')" % (self.dbname, self.jsonTemplate, self.jsonTemplate1)
        print(createSql)
        tdSql.execute(createSql)
        tdSql.query(f'desc t1')
        tdSql.checkRows(4)
        tdSql.checkData(0, 7, '')
        tdSql.checkData(1, 7, '')
        tdSql.checkData(2, 7, '1:{"k1":"string","k2":"long","k3":["double"],"k4":"long","k5":{"k6":"boolean","k7":"double"},"k8":"string"}')
        tdSql.checkData(3, 7, '1:{"k1":"string","k2":"long"}')

    def check_alter_normal_table_add_template_error(self):
        tdSql.error("alter table t1 modify column c2")
        tdSql.error("alter table t1 modify column c2 add template")
        tdSql.error("alter table t1 modify column c2 add template ''")
        tdSql.error("alter table t1 modify column c2 add template 'dadf'")
        tdSql.error("alter table t1 modify column c2 add template '\"dadf\"'")
        tdSql.error("alter table t1 modify column c2 add template 343")
        tdSql.error("alter table t1 modify column c2 add template ' '")
        tdSql.error("alter table t1 modify column c2 add template 'true'")
        tdSql.error("alter table t1 modify column c2 add template false")
        tdSql.error("alter table t1 modify column c2 add template '[]'")
        tdSql.error("alter table t1 modify column c2 add template '{}'")
        tdSql.error("alter table t1 modify column c2 add template {}")
        tdSql.error("alter table t1 modify column c2 add template null")
        tdSql.error("alter table t1 modify column c2 add template 'null'")
        tdSql.error("alter table t1 modify column c1 add template ''")
        tdSql.error('''alter table t1 modify column c1 add template '{"k1":"string"}' ''')
        tdSql.error('''alter table t1 modify column c2 template '{"k1":"string"}' ''')
        tdSql.error('alter table t1 modify column c2 nchar(20)')
        tdSql.execute('alter table t1 rename column c2 c22')
        tdSql.execute('alter table t1 rename column c22 c2')

    def check_alter_normal_table_add_template_ok(self):
        alterSql = '''alter table t1 modify column c2 add template '{"k1":"string","k2":"boolean"}' '''
        tdSql.execute(alterSql)
        tdSql.error(alterSql)

        tdSql.query(f'desc t1')
        tdSql.checkData(2, 7, '1:{"k1":"string","k2":"long","k3":["double"],"k4":"long","k5":{"k6":"boolean","k7":"double"},"k8":"string"},2:{"k1":"string","k2":"boolean"}')

    def check_alter_normal_table_drop_template_error(self):
        tdSql.error('''alter table t1 modify column c2 drop template "1" ''')
        tdSql.error('''alter table t1 modify column c2 drop template "" ''')
        tdSql.error('''alter table t1 modify column c2 drop template "heelo" ''')
        tdSql.error('''alter table t1 modify column c2 drop template -1 ''')
        tdSql.error('''alter table t1 modify column c2 drop template 0 ''')
        tdSql.error('''alter table t1 modify column c2 drop template 3 ''')
        tdSql.error('''alter table t1 modify column c1 drop template 1 ''')

        tdSql.error('''alter table t1 modify column c3 drop template 1 ''')

    def check_alter_normal_table_drop_template_ok(self):
        alterSql = '''alter table t1 modify column c2 drop template 2'''
        tdSql.execute(alterSql)
        tdSql.error(alterSql)

        tdSql.query(f'desc t1')
        tdSql.checkData(2, 7, '1:{"k1":"string","k2":"long","k3":["double"],"k4":"long","k5":{"k6":"boolean","k7":"double"},"k8":"string"}')
        tdSql.error('''alter table t1 modify column c2 drop template 1 ''')

    def check_alter_normal_table_add_template_again_ok(self):
        alterSql = '''alter table t1 modify column c2 add template '{"k1":"string","k2":"boolean"}' '''
        tdSql.execute(alterSql)

        tdSql.query(f'desc t1')
        tdSql.checkData(2, 7, '1:{"k1":"string","k2":"long","k3":["double"],"k4":"long","k5":{"k6":"boolean","k7":"double"},"k8":"string"},3:{"k1":"string","k2":"boolean"}')

    def check_show_normal_table_ok(self):
        tdSql.query(f'show create table t1')
        tdSql.checkData(0, 1, '''CREATE TABLE `t1` (`ts` TIMESTAMP ENCODE 'delta-i' COMPRESS 'lz4' LEVEL 'medium', `c1` INT ENCODE 'simple8b' COMPRESS 'lz4' LEVEL 'medium', `c2` JSON ENCODE 'disabled' COMPRESS 'lz4' LEVEL 'medium' TEMPLATE '{"k1":"string","k2":"long","k3":["double"],"k4":"long","k5":{"k6":"boolean","k7":"double"},"k8":"string"}', `c3` JSON ENCODE 'disabled' COMPRESS 'lz4' LEVEL 'medium' TEMPLATE '{"k1":"string","k2":"long"}')''')

    def check_alter_normal_table_add_drop_json_column(self):
        tdSql.error('''alter table t1 add column c4 int template''')
        tdSql.error('''alter table t1 add column c4 int template '{"k1":"string"}' ''')
        tdSql.error('''alter table t1 add column c4 json''')
        tdSql.error('''alter table t1 add column c4 json template '' ''')
        tdSql.error('''alter table t1 add column c4 json template '"hello"' ''')
        tdSql.error('''alter table t1 add column c4 json template '{}' ''')

        tdSql.execute('''alter table t1 add column c4 json template '{"k1":"string"}' ''')
        tdSql.query(f'desc t1')
        tdSql.checkData(4, 7, '1:{"k1":"string"}')

        #Row length exceeds max length 65531
        tdSql.error('''alter table t1 add column c5 json template '{"k1":"string"}' ''')

        tdSql.execute("create table tt(ts timestamp, c1 int)")
        tdSql.execute('''alter table tt add column c4 json template '{"k1":"string"}' ''')
        tdSql.query(f'desc tt')
        tdSql.checkData(2, 7, '1:{"k1":"string"}')

        tdSql.execute('''alter table tt drop column c4 ''')
        tdSql.query(f'desc tt')
        tdSql.checkRows(2)

        tdSql.execute('''drop table tt ''')
        tdSql.error(f'desc tt')

    def check_create_normal_table_result(self):
        self.check_create_normal_table_error()
        self.check_create_normal_table_ok()
        self.check_alter_normal_table_add_template_error()
        self.check_alter_normal_table_add_template_ok()
        self.check_alter_normal_table_drop_template_error()
        self.check_alter_normal_table_drop_template_ok()
        self.check_alter_normal_table_add_template_again_ok()
        self.check_show_normal_table_ok()
        self.check_alter_normal_table_add_drop_json_column()

    def check_insert_normal_table_result(self):
        tdSql.execute(''' insert into t1(ts,c2) values(now,'{"Cmd":2,"Encrypt":1,"Vin":"1G1BL52P7TR115520","Data":{"Infos":[{"Motors":[{"CtrlTemp":125,"DCBusCurrent":31203,"InputVoltage":30012,"MotorTemp":125,"No":1,"Rotating":30000,"Status":1,"Torque":25000},{"CtrlTemp":125,"DCBusCurrent":30200,"InputVoltage":32000,"MotorTemp":145,"No":2,"Rotating":30200,"Status":1,"Torque":25300}],"Number":2,"Type":"DriveMotor"}],"Time":{"Day":1,"Hour":2,"Minute":59,"Month":1,"Second":0,"Year":16}}}') ''')
        tdSql.execute(''' insert into t1(ts,c2) values(now + 1s,'{"k1":"stringk1","k2":123,"k3":[1.5,2.5],"k4":123,"k5":{"k6":true,"k7":1.5},"k8":"stringk8"}') ''')
        tdSql.execute(''' insert into t1(ts,c2) values(now + 2s,'{"k1":"中国","k2":123,"k3":[1.5,2.5],"k4":123,"k5":{"k6":true,"k7":1.5},"k8":"stringk8"}') ''')
        tdSql.execute(''' insert into t1(ts,c4) values(now + 3s,'{"k1":"中国"}') ''')
        tdSql.execute(''' insert into t1 values(now + 4s, 1, '{"k1":"stringk1","k2":123,"k3":[1.5,2.5],"k4":123,"k5":{"k6":true,"k7":1.5},"k8":"stringk8"}', 
        '{"k1":"string","k2":"long"}',
        '{"k1":"中国"}') ''')
        tdSql.execute(''' insert into t1 values(now + 5s, 1, '"212"', '"34"', '"eue"') ''')

        tdSql.query(f'select * from t1')
        tdSql.checkData(0, 2, '''{"Cmd":2,"Encrypt":1,"Vin":"1G1BL52P7TR115520","Data":{"Infos":[{"Motors":[{"CtrlTemp":125,"DCBusCurrent":31203,"InputVoltage":30012,"MotorTemp":125,"No":1,"Rotating":30000,"Status":1,"Torque":25000},{"CtrlTemp":125,"DCBusCurrent":30200,"InputVoltage":32000,"MotorTemp":145,"No":2,"Rotating":30200,"Status":1,"Torque":25300}],"Number":2,"Type":"DriveMotor"}],"Time":{"Day":1,"Hour":2,"Minute":59,"Month":1,"Second":0,"Year":16}}}''')
        tdSql.checkData(0, 3, None)
        tdSql.checkData(1, 2, '{"k1":"stringk1","k2":123,"k3":[1.5,2.5],"k4":123,"k5":{"k6":true,"k7":1.5},"k8":"stringk8"}')
        tdSql.checkData(1, 3, None)
        tdSql.checkData(2, 2, '{"k1":"中国","k2":123,"k3":[1.5,2.5],"k4":123,"k5":{"k6":true,"k7":1.5},"k8":"stringk8"}')
        tdSql.checkData(2, 3, None)

        tdSql.checkData(3, 1, None)
        tdSql.checkData(3, 2, None)
        tdSql.checkData(3, 3, None)
        tdSql.checkData(3, 4, '{"k1":"中国"}')

        tdSql.checkData(4, 1, 1)
        tdSql.checkData(4, 2, '{"k1":"stringk1","k2":123,"k3":[1.5,2.5],"k4":123,"k5":{"k6":true,"k7":1.5},"k8":"stringk8"}')
        tdSql.checkData(4, 3, '{"k1":"string","k2":"long"}')
        tdSql.checkData(4, 4, '{"k1":"中国"}')

        tdSql.checkData(5, 1, 1)
        tdSql.checkData(5, 2, '"212"')
        tdSql.checkData(5, 3, '"34"')
        tdSql.checkData(5, 4, '"eue"')

    def check_create_super_table_result(self):
        createSql = "create table %s.t2(ts timestamp, c1 int, c2 json template '%s',c3 json template '%s') tags(t int)" % (self.dbname, self.jsonTemplate, self.jsonTemplate1)
        print(createSql)
        tdSql.execute(createSql)
        tdSql.query(f'desc t2')
        tdSql.checkRows(5)
        tdSql.checkData(0, 7, '')
        tdSql.checkData(1, 7, '')
        tdSql.checkData(2, 7, '1:{"k1":"string","k2":"long","k3":["double"],"k4":"long","k5":{"k6":"boolean","k7":"double"},"k8":"string"}')
        tdSql.checkData(3, 7, '1:{"k1":"string","k2":"long"}')
        tdSql.checkData(4, 7, '')

        alterSql = '''alter table t2 modify column c2 add template '{"k1":"string","k2":"boolean"}' '''
        tdSql.execute(alterSql)

        tdSql.query(f'desc t2')
        tdSql.checkData(2, 7, '1:{"k1":"string","k2":"long","k3":["double"],"k4":"long","k5":{"k6":"boolean","k7":"double"},"k8":"string"},2:{"k1":"string","k2":"boolean"}')

        tdSql.error('''alter table t2 modify column c2 add template '{"k1":"string","k2":"boolean"}' ''')
        tdSql.error('''alter table t2 modify column c2 add template '{"k1":"string","k2":"booean"}' ''')
        tdSql.error('''alter table t2 modify column c2 add template 'fasd' ''')
        tdSql.error('''alter table t2 modify column c2 add template 4 ''')

        tdSql.error('''alter table t2 modify column c2 drop template "1" ''')

        alterSql = '''alter table t2 modify column c2 drop template 2'''
        tdSql.execute(alterSql)

        tdSql.query(f'desc t2')
        tdSql.checkData(2, 7, '1:{"k1":"string","k2":"long","k3":["double"],"k4":"long","k5":{"k6":"boolean","k7":"double"},"k8":"string"}')

        alterSql = '''alter table t2 modify column c2 add template '{"k1":"string","k2":"boolean"}' '''
        tdSql.execute(alterSql)

        tdSql.query(f'desc t2')
        tdSql.checkData(2, 7, '1:{"k1":"string","k2":"long","k3":["double"],"k4":"long","k5":{"k6":"boolean","k7":"double"},"k8":"string"},3:{"k1":"string","k2":"boolean"}')

        tdSql.query(f'show create table t2')
        tdSql.checkData(0, 1, '''CREATE STABLE `t2` (`ts` TIMESTAMP ENCODE 'delta-i' COMPRESS 'lz4' LEVEL 'medium', `c1` INT ENCODE 'simple8b' COMPRESS 'lz4' LEVEL 'medium', `c2` JSON ENCODE 'disabled' COMPRESS 'lz4' LEVEL 'medium' TEMPLATE '{"k1":"string","k2":"long","k3":["double"],"k4":"long","k5":{"k6":"boolean","k7":"double"},"k8":"string"}', `c3` JSON ENCODE 'disabled' COMPRESS 'lz4' LEVEL 'medium' TEMPLATE '{"k1":"string","k2":"long"}') TAGS (`t` INT)''')


    def run(self):
        tdSql.execute(f'create database {self.dbname}')
        tdSql.execute(f'use {self.dbname}')

        self.check_create_normal_table_result()
        self.check_insert_normal_table_result()
        self.check_create_super_table_result()

    def stop(self):
        tdSql.close()
        tdLog.success(f"{__file__} successfully executed")

tdCases.addLinux(__file__, TDTestCase())
tdCases.addWindows(__file__, TDTestCase())
