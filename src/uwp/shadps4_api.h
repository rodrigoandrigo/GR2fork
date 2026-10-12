#pragma once

#include <stddef.h>
#include <stdint.h>

#ifdef SHADPS4_API_EXPORTS
#define SHADPS4_API __declspec(dllexport)
#else
#define SHADPS4_API __declspec(dllimport)
#endif

#ifdef __cplusplus
extern "C" {
#endif

enum ShadPS4State {
    SHADPS4_UNINITIALIZED,
    SHADPS4_READY,
    SHADPS4_RUNNING,
    SHADPS4_STOPPED,
    SHADPS4_FAILED,
};

typedef long(__cdecl* ShadPS4AttachSwapChain)(void* context, void* swapchain);
typedef void(__cdecl* ShadPS4LogCallback)(void* context, uint32_t level, const char* message);
typedef void(__cdecl* ShadPS4ErrorCallback)(void* context, const char* message);

enum ShadPS4FileMode {
    SHADPS4_FILE_READ,
    SHADPS4_FILE_READ_WRITE,
    SHADPS4_FILE_CREATE,
    SHADPS4_FILE_APPEND,
};

typedef struct ShadPS4FileInfo {
    uint32_t size;
    uint32_t is_directory;
    uint64_t length;
    int64_t modified_seconds;
} ShadPS4FileInfo;

typedef void(__cdecl* ShadPS4DirectoryCallback)(void* context, const char* name,
                                                const ShadPS4FileInfo* info);

typedef struct ShadPS4Config {
    uint32_t size;
    uint32_t version;
    const wchar_t* user_directory;
    void* panel;
    uint32_t width;
    uint32_t height;
    ShadPS4AttachSwapChain attach_swapchain;
    void* attach_context;
} ShadPS4Config;

SHADPS4_API uint32_t __cdecl shadps4_get_api_version(void);
SHADPS4_API void __cdecl shadps4_set_log_callback(ShadPS4LogCallback callback, void* context);
SHADPS4_API void __cdecl shadps4_set_error_callback(ShadPS4ErrorCallback callback, void* context);
SHADPS4_API int32_t __cdecl shadps4_shutdown(void);
SHADPS4_API void __cdecl shadps4_flush_logs(void);
SHADPS4_API int32_t __cdecl shadps4_check_environment(void);
SHADPS4_API int32_t __cdecl shadps4_initialize(const ShadPS4Config* config);
SHADPS4_API int32_t __cdecl shadps4_run(const wchar_t* executable);
SHADPS4_API void __cdecl shadps4_request_stop(void);
SHADPS4_API void __cdecl shadps4_set_paused(int32_t paused);
SHADPS4_API int32_t __cdecl shadps4_set_host_state(int32_t active, int32_t input_captured);
SHADPS4_API void __cdecl shadps4_resize(uint32_t width, uint32_t height);
SHADPS4_API int32_t __cdecl shadps4_render_host_frame(void);
SHADPS4_API uint32_t __cdecl shadps4_get_state(void);
SHADPS4_API uint32_t __cdecl shadps4_get_error(char* destination, uint32_t capacity);
SHADPS4_API int32_t __cdecl shadps4_mount_storage_folder(const char* mount, void* folder,
                                                         int32_t read_only);
SHADPS4_API int32_t __cdecl shadps4_unmount_storage_folder(const char* mount);
SHADPS4_API int32_t __cdecl shadps4_open_storage_file(void* file, uint32_t mode, uint64_t* token);
SHADPS4_API int32_t __cdecl shadps4_open_random_access_stream(void* stream, uint32_t mode,
                                                              uint64_t* token);
SHADPS4_API int32_t __cdecl shadps4_open_file_handle(void* handle, uint32_t mode, uint64_t* token);
SHADPS4_API int32_t __cdecl shadps4_open_mounted_file(const char* path, uint32_t mode,
                                                      uint64_t* token);
SHADPS4_API int32_t __cdecl shadps4_file_read(uint64_t token, void* data, uint64_t size,
                                              uint64_t* transferred);
SHADPS4_API int32_t __cdecl shadps4_file_write(uint64_t token, const void* data, uint64_t size,
                                               uint64_t* transferred);
SHADPS4_API int32_t __cdecl shadps4_file_seek(uint64_t token, int64_t offset, uint32_t origin,
                                              uint64_t* position);
SHADPS4_API int32_t __cdecl shadps4_file_get_size(uint64_t token, uint64_t* size);
SHADPS4_API int32_t __cdecl shadps4_file_set_size(uint64_t token, uint64_t size);
SHADPS4_API int32_t __cdecl shadps4_file_flush(uint64_t token);
SHADPS4_API int32_t __cdecl shadps4_file_close(uint64_t token);
SHADPS4_API int32_t __cdecl shadps4_storage_stat(const char* path, ShadPS4FileInfo* info);
SHADPS4_API int32_t __cdecl shadps4_storage_enumerate(const char* path,
                                                      ShadPS4DirectoryCallback callback,
                                                      void* context);
SHADPS4_API int32_t __cdecl shadps4_storage_create_directory(const char* path);
SHADPS4_API int32_t __cdecl shadps4_storage_delete(const char* path);
SHADPS4_API int32_t __cdecl shadps4_storage_rename(const char* from, const char* to);
SHADPS4_API int32_t __cdecl shadps4_get_settings(char* data, uint32_t capacity, uint32_t* required);
SHADPS4_API int32_t __cdecl shadps4_set_setting(const char* group, const char* key,
                                                const char* value_json);
SHADPS4_API int32_t __cdecl shadps4_reset_settings(void);
SHADPS4_API int32_t __cdecl shadps4_get_game_settings(const char* serial, char* data,
                                                      uint32_t capacity, uint32_t* required);
SHADPS4_API int32_t __cdecl shadps4_set_game_setting(const char* serial, const char* group,
                                                     const char* key, const char* value_json);
SHADPS4_API int32_t __cdecl shadps4_delete_game_settings(const char* serial);

#ifdef __cplusplus
}
#endif
