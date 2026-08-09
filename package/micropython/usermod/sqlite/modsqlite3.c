// SPDX-License-Identifier: MIT

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include <sqlite3.h>

#include "py/mpthread.h"
#include "py/runtime.h"

#define SQLITE_SCALAR_MAX_BYTES 255
#define SQLITE_ERROR_MAX_BYTES 127

typedef struct {
    mp_obj_base_t base;
    sqlite3 *database;
    bool busy;
} sqlite_connection_obj_t;

static const mp_obj_type_t sqlite_connection_type;

static void copy_error(char *destination, size_t size, sqlite3 *database, int result) {
    const char *source = database == NULL ? sqlite3_errstr(result) : sqlite3_errmsg(database);
    if (source == NULL) {
        source = "unknown error";
    }
    strncpy(destination, source, size - 1);
    destination[size - 1] = '\0';
}

static MP_NORETURN void raise_sqlite_error(const char *message) {
    mp_raise_msg_varg(&mp_type_RuntimeError, MP_ERROR_TEXT("sqlite3: %s"), message);
}

static sqlite3 *connection_begin(sqlite_connection_obj_t *self) {
    if (self->database == NULL) {
        mp_raise_ValueError(MP_ERROR_TEXT("connection is closed"));
    }
    if (self->busy) {
        mp_raise_msg(&mp_type_RuntimeError, MP_ERROR_TEXT("connection is busy"));
    }
    self->busy = true;
    return self->database;
}

static mp_obj_t connection_close(mp_obj_t self_in) {
    sqlite_connection_obj_t *self = MP_OBJ_TO_PTR(self_in);
    if (self->database == NULL) {
        return mp_const_none;
    }
    if (self->busy) {
        mp_raise_msg(&mp_type_RuntimeError, MP_ERROR_TEXT("connection is busy"));
    }

    sqlite3 *database = self->database;
    self->busy = true;
    MP_THREAD_GIL_EXIT();
    int result = sqlite3_close_v2(database);
    MP_THREAD_GIL_ENTER();
    self->busy = false;
    if (result != SQLITE_OK) {
        char error[SQLITE_ERROR_MAX_BYTES + 1];
        copy_error(error, sizeof(error), database, result);
        raise_sqlite_error(error);
    }
    self->database = NULL;
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_1(connection_close_obj, connection_close);

static mp_obj_t connection_execute(mp_obj_t self_in, mp_obj_t sql_in) {
    sqlite_connection_obj_t *self = MP_OBJ_TO_PTR(self_in);
    const char *sql = mp_obj_str_get_str(sql_in);
    sqlite3 *database = connection_begin(self);

    MP_THREAD_GIL_EXIT();
    int result = sqlite3_exec(database, sql, NULL, NULL, NULL);
    MP_THREAD_GIL_ENTER();
    self->busy = false;
    if (result != SQLITE_OK) {
        char error[SQLITE_ERROR_MAX_BYTES + 1];
        copy_error(error, sizeof(error), database, result);
        raise_sqlite_error(error);
    }
    return self_in;
}
static MP_DEFINE_CONST_FUN_OBJ_2(connection_execute_obj, connection_execute);

static mp_obj_t connection_scalar(mp_obj_t self_in, mp_obj_t sql_in) {
    sqlite_connection_obj_t *self = MP_OBJ_TO_PTR(self_in);
    const char *sql = mp_obj_str_get_str(sql_in);
    sqlite3 *database = connection_begin(self);
    sqlite3_stmt *statement = NULL;
    int result;
    int column_type = SQLITE_NULL;
    int64_t integer_value = 0;
    double float_value = 0;
    uint8_t byte_value[SQLITE_SCALAR_MAX_BYTES];
    int byte_count = 0;
    char error[SQLITE_ERROR_MAX_BYTES + 1] = {0};

    MP_THREAD_GIL_EXIT();
    result = sqlite3_prepare_v2(database, sql, -1, &statement, NULL);
    if (result == SQLITE_OK) {
        result = sqlite3_step(statement);
    }
    if (result == SQLITE_ROW) {
        column_type = sqlite3_column_type(statement, 0);
        switch (column_type) {
            case SQLITE_INTEGER:
                integer_value = sqlite3_column_int64(statement, 0);
                break;
            case SQLITE_FLOAT:
                float_value = sqlite3_column_double(statement, 0);
                break;
            case SQLITE_TEXT:
            case SQLITE_BLOB: {
                const void *source = column_type == SQLITE_TEXT
                    ? (const void *)sqlite3_column_text(statement, 0)
                    : sqlite3_column_blob(statement, 0);
                byte_count = sqlite3_column_bytes(statement, 0);
                if (byte_count <= SQLITE_SCALAR_MAX_BYTES && byte_count > 0) {
                    memcpy(byte_value, source, (size_t)byte_count);
                }
                break;
            }
            default:
                break;
        }
    } else if (result != SQLITE_DONE) {
        copy_error(error, sizeof(error), database, result);
    }
    if (statement != NULL) {
        sqlite3_finalize(statement);
    }
    MP_THREAD_GIL_ENTER();
    self->busy = false;

    if (result != SQLITE_ROW && result != SQLITE_DONE) {
        raise_sqlite_error(error);
    }
    if (result == SQLITE_DONE || column_type == SQLITE_NULL) {
        return mp_const_none;
    }
    if (byte_count > SQLITE_SCALAR_MAX_BYTES) {
        mp_raise_ValueError(MP_ERROR_TEXT("scalar value exceeds 255 bytes"));
    }
    switch (column_type) {
        case SQLITE_INTEGER:
            return mp_obj_new_int_from_ll(integer_value);
        case SQLITE_FLOAT:
            return mp_obj_new_float(float_value);
        case SQLITE_TEXT:
            return mp_obj_new_str((const char *)byte_value, (size_t)byte_count);
        case SQLITE_BLOB:
            return mp_obj_new_bytes(byte_value, (size_t)byte_count);
        default:
            return mp_const_none;
    }
}
static MP_DEFINE_CONST_FUN_OBJ_2(connection_scalar_obj, connection_scalar);

static const mp_rom_map_elem_t connection_locals_table[] = {
    { MP_ROM_QSTR(MP_QSTR_close), MP_ROM_PTR(&connection_close_obj) },
    { MP_ROM_QSTR(MP_QSTR___del__), MP_ROM_PTR(&connection_close_obj) },
    { MP_ROM_QSTR(MP_QSTR_execute), MP_ROM_PTR(&connection_execute_obj) },
    { MP_ROM_QSTR(MP_QSTR_scalar), MP_ROM_PTR(&connection_scalar_obj) },
};
static MP_DEFINE_CONST_DICT(connection_locals, connection_locals_table);

static MP_DEFINE_CONST_OBJ_TYPE(
    sqlite_connection_type,
    MP_QSTR_SQLiteConnection,
    MP_TYPE_FLAG_NONE,
    locals_dict, &connection_locals
    );

static mp_obj_t sqlite_connect(mp_obj_t path_in) {
    const char *path = mp_obj_str_get_str(path_in);
    sqlite_connection_obj_t *self = mp_obj_malloc_with_finaliser(
        sqlite_connection_obj_t, &sqlite_connection_type);
    self->database = NULL;
    self->busy = true;

    sqlite3 *database = NULL;
    MP_THREAD_GIL_EXIT();
    int result = sqlite3_open_v2(path, &database,
        SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE | SQLITE_OPEN_FULLMUTEX, NULL);
    if (result == SQLITE_OK) {
        result = sqlite3_busy_timeout(database, 30000);
    }
    if (result != SQLITE_OK) {
        char error[SQLITE_ERROR_MAX_BYTES + 1];
        copy_error(error, sizeof(error), database, result);
        if (database != NULL) {
            sqlite3_close_v2(database);
        }
        MP_THREAD_GIL_ENTER();
        self->busy = false;
        raise_sqlite_error(error);
    }
    MP_THREAD_GIL_ENTER();

    self->database = database;
    self->busy = false;
    return MP_OBJ_FROM_PTR(self);
}
static MP_DEFINE_CONST_FUN_OBJ_1(sqlite_connect_obj, sqlite_connect);

static mp_obj_t sqlite_version(void) {
    const char *version = sqlite3_libversion();
    return mp_obj_new_str(version, strlen(version));
}
static MP_DEFINE_CONST_FUN_OBJ_0(sqlite_version_obj, sqlite_version);

static const mp_rom_map_elem_t sqlite_module_globals_table[] = {
    { MP_ROM_QSTR(MP_QSTR___name__), MP_ROM_QSTR(MP_QSTR_sqlite3) },
    { MP_ROM_QSTR(MP_QSTR_connect), MP_ROM_PTR(&sqlite_connect_obj) },
    { MP_ROM_QSTR(MP_QSTR_version), MP_ROM_PTR(&sqlite_version_obj) },
};
static MP_DEFINE_CONST_DICT(sqlite_module_globals, sqlite_module_globals_table);

const mp_obj_module_t sqlite_user_module = {
    .base = { &mp_type_module },
    .globals = (mp_obj_dict_t *)&sqlite_module_globals,
};

MP_REGISTER_MODULE(MP_QSTR_sqlite3, sqlite_user_module);
