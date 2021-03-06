#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <msgpack.h>

#include "rpc.h"
#include "pydirty.h"
#include "unpack.h"
#include "log.h"
#include "util.h"
#include "script.h"

static rpc_function_table_t g_function_rpc_table;
static rpc_struct_table_t g_struct_rpc_table;

PyObject * unpack(rpc_function_t *function, msgpack_unpacker_t *unpacker);
int pack_field(rpc_field_t *field, PyObject *item, msgpack_packer *pck);
PyObject *unpack_field(rpc_field_t *field, msgpack_unpacker_t *unpacker);

rpc_struct_t * get_struct_by_id(int struct_id)
{
    if (struct_id <= 0 || struct_id > g_struct_rpc_table.size) return NULL;
    return g_struct_rpc_table.table + struct_id - 1;
}

rpc_function_t * get_function_by_id(int function_id)
{
    if (function_id <= 0 || function_id > g_function_rpc_table.size) return NULL;
    return g_function_rpc_table.table + function_id - 1;
}

int pack_struct(rpc_struct_t *pstruct, PyObject *obj, msgpack_packer *pck)
{
    rpc_field_t *field;
    PyObject *item;
    PyObject *key;
    if (!PyDict_CheckExact(obj) && !PyDirtyDict_CheckExact(obj)) {
        fprintf(stderr, "pack struct obj must be dict.\n");
        assert(0);
    }

    for (int i = 0; i < pstruct->field_cnt; i++) {
        field = pstruct->field_list + i;
        key = PyUnicode_FromString(field->name);
        item = PyDict_GetItem(obj, key);
        Py_DECREF(key);
        if (item == NULL) {
            fprintf(stderr, "pack struct error lack of field. field_name:%s\n", field->name);
            return -1;
        }

        if (field->array == 1) {
            if (!PyList_CheckExact(item)) {
                fprintf(stderr, "expected field type is list\n");
                return -1;
            }
            Py_ssize_t size = PyList_Size(item);
            msgpack_pack_int(pck, size); 
            for (int j = 0; j < size; j++) {
                if (pack_field(field, PyList_GetItem(item, j), pck) == -1) return -1;
            }

        } else {
            if (pack_field(field, item, pck) == -1) return -1;
        }
    }
    return 0;
}

int check_field_type(int field_type, PyObject *item)
{
    switch(field_type) {
        case RPC_INT32:
            if (!PyLong_CheckExact(item)) return -1;
            break;
        case RPC_STRING:
            if (!PyUnicode_CheckExact(item)) return -1;
            break;
        case RPC_FLOAT:
            if (!PyFloat_CheckExact(item)) return -1;
            break;
        case RPC_STRUCT:
            if (!PyDict_CheckExact(item) && !PyDirtyDict_CheckExact(item)) return -1;
            break;
        default:
            fprintf(stderr, "unknown field type=%d", field_type);
            return -1;
    }
    return 0;
}

int pack_field(rpc_field_t *field, PyObject *item, msgpack_packer *pck)
{
    if (check_field_type(field->type, item) == -1) {
        fprintf(stderr, "pack field type error. expected_type=%d,type_name=%s\n", field->type, Py_TYPE(item)->tp_name);
        return -1;
    }

    switch(field->type) {
        case RPC_INT32:
            msgpack_pack_uint32(pck, (uint32_t)PyLong_AsLong(item));
            break;
        case RPC_STRING: 
            {
                Py_ssize_t size; 
                const char *str = PyUnicode_AsUTF8AndSize(item, &size);
                msgpack_pack_str(pck, size); 
                msgpack_pack_str_body(pck, str, size); 
                break;
            }
        case RPC_FLOAT:
            msgpack_pack_double(pck, PyFloat_AS_DOUBLE(item));
            break;
        case RPC_STRUCT:
            {
                rpc_struct_t *pstruct = get_struct_by_id(field->struct_id);
                if (pstruct == NULL) {
                    fprintf(stderr, "field struct id invalid\n");
                    return -1;
                }
                pack_struct(pstruct, item, pck);
            }
            break;
        default:
            fprintf(stderr, "unknown field type=%d", field->type);
            return -1;
    }
    return 0;
}

int pack_args(rpc_args_t *args, PyObject *obj, msgpack_packer *pck)
{
    assert(PyTuple_CheckExact(obj));
    if (PyTuple_Size(obj) != args->arg_cnt) {
        fprintf(stderr, "pack error: tuple obj size is not equal to args cnt\n");
        return -1;
    }

    rpc_field_t *field;
    PyObject *item;

    for (int i = 0; i < args->arg_cnt; i++) {
        field = args->arg_list + i;
        item = PyTuple_GetItem(obj, i);
        
        if (field->array == 1) {
            if (!PyList_CheckExact(item)) {
                fprintf(stderr, "pack args expected field type is list\n");
                return -1;
            }
            Py_ssize_t size = PyList_Size(item);
            msgpack_pack_unsigned_int(pck, size); 
            for (int i = 0; i < args->arg_cnt; i++) {
                if (pack_field(field, item, pck) != 0) return -1;
            }

        } else {
            if (pack_field(field, item, pck) != 0) return -1;
        }
    }
    return 0;
}

//pack obj data to buf
int pack(int pid, PyObject *obj, msgpack_sbuffer *sbuf)
{
    if (!PyTuple_CheckExact(obj)) {
        fprintf(stderr, "expected pack obj is tuple\n");
        return -1; 
    }

    rpc_function_t * function = get_function_by_id(pid);
    if (function == NULL) {
        fprintf(stderr, "can't find pid function. pid=%d\n", pid);
        return -1;
    }

    msgpack_sbuffer_init(sbuf);
    msgpack_packer pck;
    msgpack_packer_init(&pck, sbuf, msgpack_sbuffer_write);

    if (function->args.arg_cnt != PyTuple_Size(obj)) {
        fprintf(stderr, "tuple obj size is not equal arg cnt. expected=%d, size=%ld\n", function->args.arg_cnt, PyTuple_Size(obj));
        return -1;
    }

    return pack_args(&function->args, obj, &pck);
}

//rpc unpack start
PyObject *unpack_struct(rpc_struct_t *pstruce, msgpack_unpacker_t *unpacker)
{
    rpc_field_t *field;
    PyObject *key, *value, *item;
    msgpack_unpack_return ret;
    PyObject *dict = PyDict_New();
    if (dict == NULL) return NULL;

    for (int i = 0; i < pstruce->field_cnt; i++) {
        field = pstruce->field_list + i;
        fprintf(stderr, "unpack struct array field name!!!%s\n", field->name);
        if (field->array == 1) {
            //先取出数组
            ret = msgpck_unpacker_next_pack(unpacker);
            if (ret == MSGPACK_UNPACK_SUCCESS) {
                if (GET_UNPACKER_PACK_TYPE(unpacker) != MSGPACK_OBJECT_POSITIVE_INTEGER) {
                    fprintf(stderr, "unpack struct expected field is array\n");
                    goto error;
                }
                int size = unpacker->pack.data.via.u64;
                value = PyTuple_New(size);
                if (value == NULL) { 
                    goto error;
                }
                for (int j = 0; j < size; j++) {
                    item = unpack_field(field, unpacker); 
                    if (item == NULL) {
                        goto error;
                    }
                    if (PyTuple_SetItem(value, j, item) == -1) {
                        goto error;
                    }
                }
            } else {
                fprintf(stderr, "unpack struct is error ret=%d\n", ret);
                goto error;
            }
        } else {
            value = unpack_field(field, unpacker); 
            if (value == NULL) {
                goto error;
            }
        }
        
        key = PyUnicode_FromString(field->name);
        if (PyDict_SetItem(dict, key, value) == 0) {
            Py_DECREF(key);
            Py_DECREF(value);
        } else {
            Py_DECREF(key);
            Py_DECREF(value);
            goto error;
        }
    }
    return dict;
error:
    Py_DECREF(dict);
    return NULL; 
}


//return obj 正常
//return null 异常
PyObject *unpack_field(rpc_field_t *field, msgpack_unpacker_t *unpacker)
{
    msgpack_unpack_return ret;
    switch(field->type) {
        case RPC_INT32:
            ret = msgpck_unpacker_next_pack(unpacker);
            if (ret != MSGPACK_UNPACK_SUCCESS) {
                goto unpack_error;
            }
            if (unpacker->pack.data.type != MSGPACK_OBJECT_POSITIVE_INTEGER) { 
                printf("unpack field type error. expected type is int, but get type:%d, field_name=%s\n", unpacker->pack.data.type, field->name);
                return NULL;
            }
            return PyLong_FromLong(unpacker->pack.data.via.u64);
        case RPC_STRING: 
            ret = msgpck_unpacker_next_pack(unpacker);
            if (ret != MSGPACK_UNPACK_SUCCESS) {
                goto unpack_error;
            }
            if (unpacker->pack.data.type != MSGPACK_OBJECT_STR) { 
                printf("unpack field type error. expected type is string, but get type:%d, field_name=%s\n", unpacker->pack.data.type, field->name);
                return NULL;
            }
            return PyUnicode_FromStringAndSize(unpacker->pack.data.via.str.ptr, unpacker->pack.data.via.str.size);
        case RPC_FLOAT:
            ret = msgpck_unpacker_next_pack(unpacker);
            if (ret != MSGPACK_UNPACK_SUCCESS) {
                msgpack_object_print(stderr, unpacker->pack.data);
                goto unpack_error;
            }
            if (unpacker->pack.data.type != MSGPACK_OBJECT_FLOAT32) { 
                printf("unpack field type error. expected type is float, but get type:%d, field_name=%s\n", unpacker->pack.data.type, field->name);
                return NULL;
            }
            return PyFloat_FromDouble(unpacker->pack.data.via.f64);
        case RPC_STRUCT:
            {
                rpc_struct_t *pstruct = get_struct_by_id(field->struct_id);
                if (pstruct == NULL) {
                    fprintf(stderr, "field struct id invalid\n");
                    return NULL;
                }
                return unpack_struct(pstruct, unpacker);
            }
        default:
            fprintf(stderr, "unpack unknown field type=%d\n", field->type);
            return NULL;
    }

    return NULL;
unpack_error:
    fprintf(stderr, "unpack next pack error.\n");
    return NULL;
}

//把args塞入tuple
int unpack_args(rpc_args_t *args, PyObject *obj, msgpack_unpacker_t *unpacker)
{
    rpc_field_t *field;
    PyObject *value, *item;
    msgpack_unpack_return ret;

    assert(PyTuple_CheckExact(obj));

    for (int i = 0; i < args->arg_cnt; i++) {
        field = args->arg_list + i;
        if (field->array == 1) {
            //取出数组
            ret = msgpck_unpacker_next_pack(unpacker);
            if (ret == MSGPACK_UNPACK_SUCCESS) {
                if (GET_UNPACKER_PACK_TYPE(unpacker) != MSGPACK_OBJECT_POSITIVE_INTEGER) {
                    fprintf(stderr, "unpack error arg field expected is array\n");
                    return -1;
                }
                int size = unpacker->pack.data.via.u64;
                value = PyTuple_New(size);
                for (int j = 0; j < size; j++) {
                    item = unpack_field(field, unpacker); 
                    if (item == NULL) {
                        Py_DECREF(value);
                        return -1;
                    }
                    PyTuple_SetItem(value, j, item);
                }
            } else {
                fprintf(stderr, "get next pack error ret=%d\n", ret);
                return -1;
            }
        } else {
            value = unpack_field(field, unpacker);
            if (value == NULL) {
                return -1;
            }
        }
        PyTuple_SetItem(obj, i, value);
    }
    return 0;
}

//unpack buf data to obj
PyObject * unpack(rpc_function_t *function, msgpack_unpacker_t *unpacker)
{
    PyObject *obj;
    obj = PyTuple_New(function->args.arg_cnt);
    if (unpack_args(&function->args, obj, unpacker) < 0) {
        Py_DECREF(obj);
        return NULL;
    }
    return obj;
}

int rpc_c_dispatch(int id, rpc_function_t *function, msgpack_unpacker_t *unpacker)
{
    PyObject *obj = unpack(function, unpacker);
    if (obj != NULL) {
        if (function->c_function != NULL) {
            function->c_function(id, obj);
            Py_DECREF(obj);
            return 0;
        }
    }
    return -1;
}

int rpc_script_dispatch(int id, rpc_function_t *function, msgpack_unpacker_t *unpacker)
{
    PyObject *obj = unpack(function, unpacker);
    int ret;
    if (obj != NULL) {
         ret = call_script_function(function->module, function->name, obj);
         Py_DECREF(obj);
         return ret;
    }
    return -1;
}

//if return 0 success else fail
int rpc_dispatch(int id, char *buf, int len)
{
    int pid = 0;
    msgpack_unpacker_t unpacker;
    int res;

    construct_msgpack_unpacker(&unpacker, buf, len);
    if (unpacker.pack.data.type != MSGPACK_OBJECT_POSITIVE_INTEGER) {
        return -1;
    }
    pid = (int)unpacker.pack.data.via.u64;
    if (pid <= 0 || pid > g_function_rpc_table.size) {
        fprintf(stderr, "invalid rpc pid=%d,id=%d\n", pid, id);
        destroy_msgpack_unpacker(&unpacker);
        return -1;
    }

    rpc_function_t * function = g_function_rpc_table.table + pid;
    if (function->c_imp == 1) {
        res = rpc_c_dispatch(id, function, &unpacker);
    } else {
        res = rpc_script_dispatch(id, function, &unpacker);
    }

    destroy_msgpack_unpacker(&unpacker);
    return res;
}

int rpc_send(int uid, int pid, PyObject *obj)
{

}

/*
typedef enum {
    MSGPACK_OBJECT_NIL                  = 0x00,
    MSGPACK_OBJECT_BOOLEAN              = 0x01,
    MSGPACK_OBJECT_POSITIVE_INTEGER     = 0x02,
    MSGPACK_OBJECT_NEGATIVE_INTEGER     = 0x03,
    MSGPACK_OBJECT_FLOAT32              = 0x0a,
    MSGPACK_OBJECT_FLOAT64              = 0x04,
    MSGPACK_OBJECT_FLOAT                = 0x04,
    MSGPACK_OBJECT_STR                  = 0x05,
    MSGPACK_OBJECT_ARRAY                = 0x06,
    MSGPACK_OBJECT_MAP                  = 0x07,
    MSGPACK_OBJECT_BIN                  = 0x08,
    MSGPACK_OBJECT_EXT                  = 0x09
} msgpack_object_type;
*/

int create_rpc_table()
{
    FILE * fp;
    char * line = NULL;
    size_t len = 0;
    ssize_t read;

    fp = fopen("./rpc/rpc.cfg", "r");
    if (fp == NULL) {
        printf("open rpc.cfg fail\n");
        return 1;
    }
    int struct_num;
    getline(&line, &len, fp);
    sscanf(line, "struct_table_num:%d", &struct_num);
    g_struct_rpc_table.table = (rpc_struct_t *)calloc(struct_num, sizeof(rpc_struct_t));
    g_struct_rpc_table.size = struct_num;

    int struct_id;
    char struct_name[200];
    char field_name[200];
    int field_count;
    rpc_struct_t *pstruct;
    rpc_field_t *field;
    for (int i = 0; i < struct_num; i++) {
        getline(&line, &len, fp);
        assert(sscanf(line, "struct_id:%d,field_num:%d,struct_name=%s", &struct_id, &field_count, struct_name) == 3);
        pstruct = g_struct_rpc_table.table + i;
        pstruct->field_cnt = field_count;
        pstruct->field_list = calloc(field_count, sizeof(rpc_field_t));
        for (int j = 0; j < field_count; j++) {
            field = pstruct->field_list + j;
            getline(&line, &len, fp);
            assert(sscanf(line, "field_type:%d,struct_id:%d,array:%d,field_name=%s", &field->type, &field->struct_id, &field->array, field_name) == 4);
            field->name = strdup(field_name);
        }
    }

    int function_num;
    getline(&line, &len, fp);
    sscanf(line, "function_table_num:%d", &function_num);
    g_function_rpc_table.table = (rpc_function_t *)calloc(function_num, sizeof(rpc_function_t));
    g_function_rpc_table.size = function_num;

    int id;
    char name[200];
    char module[200];
    int c_imp;
    int arg_num;
    int deamon;
    rpc_function_t *function;
    for (int i = 0; i < function_num; i++) {
        getline(&line, &len, fp);
        sscanf(line, "function_id:%d,arg_num:%d,module=%[^,]%*cfunction=%s,c_imp=%d,deamon=%d", &id, &arg_num, module, name, &c_imp, &deamon);
        rpc_function_t *function = g_function_rpc_table.table + i;
        function->id = id;
        function->name = strdup(name);
        function->module = strdup(module);
        function->c_imp = c_imp;
        function->deamon = deamon;
        function->args.arg_cnt = arg_num;
        function->args.arg_list = calloc(arg_num, sizeof(rpc_field_t));
        for (int j = 0; j < arg_num; j++) {
            field = function->args.arg_list + j;
            getline(&line, &len, fp);
            sscanf(line, "arg_type:%d,struct_id:%d,array:%d", &field->type, &field->struct_id, &field->array);
        }
    }
    log_info("create rpc table success!");
    void rpc_test();
    rpc_test();
}

void rpc_test()
{
/*
    msgpack_sbuffer sbuf;
    PyObject *value = PyTuple_New(1);
    PyObject *item = PyUnicode_FromString("hello world");
    PyTuple_SetItem(value, 0, item);
    int res = pack(1, value, &sbuf);
    assert(res == 0);
    MY_PYOBJECT_PRINT(value, "test rpc11");

    printf("sbuf.data=%s,sbuf.size=%d\n", sbuf.data, sbuf.size);

    msgpack_unpacker_t unpacker;
    construct_msgpack_unpacker(&unpacker, sbuf.data, sbuf.size);
    rpc_function_t * function = g_function_rpc_table.table + 0;
    PyObject * obj = unpack(function, &unpacker);
    assert(obj != 0);
    MY_PYOBJECT_PRINT(obj, "test rpc");
*/
    msgpack_sbuffer sbuf;
    PyObject *value = PyTuple_New(2);
    PyObject *item1 = PyDict_New();
    PyDict_SetItem(item1, PyUnicode_FromString("uid"), PyLong_FromLong(12345));
    PyObject *list1 = PyList_New(1);
    PyList_SetItem(list1, 0, PyUnicode_FromString("world"));
    PyDict_SetItem(item1, PyUnicode_FromString("msg"), list1);
    //PyObject *subdict = PyDict_New();
    //PyDict_SetItem(subdict, PyUnicode_FromString("sex"), PyLong_FromLong(1));
    //PyDict_SetItem(subdict, PyUnicode_FromString("icon"), PyUnicode_FromString("1234"));
    //PyDict_SetItem(item1, PyUnicode_FromString("test_struct"), subdict);
    //PyDict_SetItem(item1, PyUnicode_FromString("test_struct"), subdict);

    PyObject *item2 = PyLong_FromLong(100);
    PyTuple_SetItem(value, 0, item1);
    PyTuple_SetItem(value, 1, item2);
    int res = pack(2, value, &sbuf);
    assert(res == 0);
    MY_PYOBJECT_PRINT(value, "test rpc11");

    printf("sbuf.data=%s,sbuf.size=%ld\n", sbuf.data, sbuf.size);

    msgpack_unpacker_t unpacker;
    construct_msgpack_unpacker(&unpacker, sbuf.data, sbuf.size);
    rpc_function_t * function = g_function_rpc_table.table + 1;
    PyObject * obj = unpack(function, &unpacker);
    assert(obj != 0);
    MY_PYOBJECT_PRINT(obj, "test rpc");
}
