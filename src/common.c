#include "kvs/common.h"

const char *kvs_status_str(kvs_status_t status) {
    switch (status) {
        case KVS_OK:
            return "Success";
        case KVS_ERR_NOMEM:
            return "Out of memory";
        case KVS_ERR_INVALID_ARG:
            return "Invalid argument (NULL or out-of-range)";
        case KVS_ERR_NOT_FOUND:
            return "Key not found";
        case KVS_ERR_KEY_EXISTS:
            return "Key already exists";
        case KVS_ERR_KEY_TOO_LONG:
            return "Key length exceeds maximum permitted limit";
        case KVS_ERR_VAL_TOO_LONG:
            return "Value length exceeds maximum permitted limit";
        case KVS_ERR_OVERFLOW:
            return "Arithmetic overflow detected";
        case KVS_ERR_DOUBLE_FREE:
            return "Double-free detected in memory pool";
        case KVS_ERR_SYS:
            return "System call failure";
        default:
            return "Unknown status code";
    }
}
