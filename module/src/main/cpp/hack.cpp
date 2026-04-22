//
// Created by Perfare on 2020/7/4.
//

#include "hack.h"
#include "il2cpp_dump.h"
#include "log.h"
#include "xdl.h"
#include <cstring>
#include <cstdio>
#include <unistd.h>
#include <sys/system_properties.h>
#include <dlfcn.h>
#include <jni.h>
#include <thread>
#include <sys/mman.h>
#include <linux/unistd.h>
#include <array>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dirent.h>
#include <sys/stat.h>
#include <vector>
#include <unordered_set>
#include <map>
#include <fstream>



// 动态路径（运行时初始化）
static std::string g_apk_path;                    // base.apk 路径
static std::string g_apk_asset_path;              // base.apk!assets/AssetBundles/Android
static std::string g_game_data_dir;               // /data/data/com.xxx
static std::string g_internal_cos_path;           // /data/data/com.xxx/files/COSABResource/Android
static std::string g_external_cos_path;           // /sdcard/Android/data/com.xxx/files/COSABResource/Android
static std::string g_update_asset_path1;          // /data/data/com.xxx/files/VersionUpdate/AssetBundles/Android
static std::string g_update_asset_path2;          // /sdcard/Android/data/com.xxx/files/VersionUpdate/AssetBundles/Android
static std::string g_exclude_path;                // /data/data/com.xxx/dump_exclude/

// 从 game_data_dir 提取包名
std::string GetPackageName(const char* game_data_dir) {
    std::string path(game_data_dir);
    size_t pos = path.rfind('/');
    if (pos != std::string::npos) {
        return path.substr(pos + 1);
    }
    return path;
}

std::string GetApkPath() {
    std::ifstream maps("/proc/self/maps");
    std::string line;
    while (std::getline(maps, line)) {
        // 查找包含 base.apk 的行
        if (line.find("base.apk") != std::string::npos) {
            // 格式: address perms offset dev inode pathname
            // 例如: 7f1234000-7f1235000 r--p 00000000 fd:00 12345 /data/app/.../base.apk
            size_t pos = line.find('/');
            if (pos != std::string::npos) {
                std::string path = line.substr(pos);
                // 移除可能的 "!classes.dex" 等后缀
                size_t excl = path.find('!');
                if (excl != std::string::npos) {
                    path = path.substr(0, excl);
                }
                // 去除尾部空白
                while (!path.empty() && (path.back() == ' ' || path.back() == '\n' || path.back() == '\r')) {
                    path.pop_back();
                }
                return path;
            }
        }
    }
    return {};
}

// 初始化所有动态路径
void InitPaths(const char* game_data_dir) {
    if (!g_game_data_dir.empty()) return; // 已初始化
    
    g_game_data_dir = game_data_dir;
    std::string package_name = GetPackageName(game_data_dir);
    
    // 获取 APK 路径
    g_apk_path = GetApkPath();
    if (!g_apk_path.empty()) {
        g_apk_asset_path = g_apk_path + "!assets/AssetBundles/Android";
        LOGD("APK asset path: %s", g_apk_asset_path.c_str());
    } else {
        LOGE("Failed to get APK path!");
    }
    
    // 设置其他路径
    g_internal_cos_path = std::string(game_data_dir) + "/files/COSABResource/Android";
    g_external_cos_path = "/sdcard/Android/data/" + package_name + "/files/COSABResource/Android";
    g_update_asset_path1 = std::string(game_data_dir) + "/files/VersionUpdate/AssetBundles/Android";
    g_update_asset_path2 = "/sdcard/Android/data/" + package_name + "/files/VersionUpdate/AssetBundles/Android";
    g_exclude_path = std::string(game_data_dir) + "/dump_exclude/";
    
    LOGD("Paths initialized:");
    LOGD("  game_data_dir: %s", g_game_data_dir.c_str());
    LOGD("  apk_path: %s", g_apk_path.c_str());
    LOGD("  apk_asset_path: %s", g_apk_asset_path.c_str());
    LOGD("  internal_cos_path: %s", g_internal_cos_path.c_str());
    LOGD("  external_cos_path: %s", g_external_cos_path.c_str());
    LOGD("  update_asset_path1: %s", g_update_asset_path1.c_str());
    LOGD("  update_asset_path2: %s", g_update_asset_path2.c_str());
    LOGD("  exclude_path: %s", g_exclude_path.c_str());
}



// ============== ZIP 文件结构定义 ==============
#pragma pack(push, 1)
struct ZipEndOfCentralDir {
    uint32_t signature;        // 0x06054b50
    uint16_t disk_number;
    uint16_t cd_disk_number;
    uint16_t cd_entries_disk;
    uint16_t cd_entries_total;
    uint32_t cd_size;
    uint32_t cd_offset;
    uint16_t comment_length;
};

struct ZipCentralDirHeader {
    uint32_t signature;        // 0x02014b50
    uint16_t version_made;
    uint16_t version_needed;
    uint16_t flags;
    uint16_t compression;
    uint16_t mod_time;
    uint16_t mod_date;
    uint32_t crc32;
    uint32_t compressed_size;
    uint32_t uncompressed_size;
    uint16_t name_length;
    uint16_t extra_length;
    uint16_t comment_length;
    uint16_t disk_start;
    uint16_t internal_attrs;
    uint32_t external_attrs;
    uint32_t local_header_offset;
};
#pragma pack(pop)

// 检查 APK 中是否存在指定的 asset 文件
// apk_path: APK 文件路径
// asset_name: 相对于 assets/ 的路径，例如 "AssetBundles/Android/xx/xxx.unity3d"
bool assetExistsInApk(const char* apk_path, const char* asset_name) {
    FILE* fp = fopen(apk_path, "rb");
    if (!fp) {
        LOGD("Failed to open apk file: %s", apk_path);
        return false;
    }
    
    // 获取文件大小
    fseek(fp, 0, SEEK_END);
    long file_size = ftell(fp);
    
    // 查找 End of Central Directory (从文件末尾向前搜索)
    ZipEndOfCentralDir eocd;
    bool found = false;
    for (long offset = sizeof(ZipEndOfCentralDir); offset < 65535 + sizeof(ZipEndOfCentralDir) && offset <= file_size; offset++) {
        fseek(fp, file_size - offset, SEEK_SET);
        if (fread(&eocd, sizeof(eocd), 1, fp) != 1) break;
        if (eocd.signature == 0x06054b50) {
            found = true;
            break;
        }
    }
    
    if (!found) {
        fclose(fp);
        LOGD("EOCD signature not found in: %s", apk_path);
        return false;
    }
    
    // 构建要查找的完整路径
    char full_name[512];
    snprintf(full_name, sizeof(full_name), "assets/%s", asset_name);
    size_t target_len = strlen(full_name);
    
    // 读取中央目录
    fseek(fp, eocd.cd_offset, SEEK_SET);
    
    char name_buf[512];
    for (uint16_t i = 0; i < eocd.cd_entries_total; i++) {
        ZipCentralDirHeader cdh;
        if (fread(&cdh, sizeof(cdh), 1, fp) != 1) break;
        if (cdh.signature != 0x02014b50) break;
        
        uint16_t name_len = cdh.name_length;
        if (name_len < sizeof(name_buf)) {
            if (fread(name_buf, name_len, 1, fp) != 1) break;
            name_buf[name_len] = '\0';
            
            if (name_len == target_len && strcmp(name_buf, full_name) == 0) {
                fclose(fp);
                return true;
            }
        } else {
            fseek(fp, name_len, SEEK_CUR);
        }
        
        // 跳过 extra 和 comment
        fseek(fp, cdh.extra_length + cdh.comment_length, SEEK_CUR);
    }
    
    fclose(fp);
    return false;
}

// 检查 APK 中是否存在指定的 asset 文件（自动获取 APK 路径版本）
bool assetExistsInApk(const char* asset_name) {
    static std::string apk_path;
    if (apk_path.empty()) {
        apk_path = GetApkPath();
        if (apk_path.empty()) {
            LOGD("Failed to get APK path");
            return false;
        }
        LOGD("APK path: %s", apk_path.c_str());
    }
    return assetExistsInApk(apk_path.c_str(), asset_name);
}


typedef int (*shadowhook_init_func)(int mode, bool debuggable);
typedef void* (*shadowhook_hook_sym_name_func)(const char *lib_name, const char *sym_name, void *new_addr, void **orig_addr);
typedef int (*shadowhook_get_errno_func)();
typedef const char * (*shadowhook_to_errmsg_func)(int error_number);

typedef std::string (*get_TKHash128_func)(std::string uri);
typedef std::map<std::string,std::string> (*get_loaded_assets_func)();
typedef void (*get_asset_info_func)(const char * path, std::string & uri, std::vector<std::string >& textures, int usleep);
typedef void (*load_asset_func)(const char * path, std::string uri, const char * texture_name, int usleep);
get_asset_info_func get_asset_info = nullptr;
load_asset_func load_asset = nullptr;
get_loaded_assets_func get_loaded_assets = nullptr;
get_TKHash128_func get_TKHash128 = nullptr;

shadowhook_init_func shadowhook_init = nullptr;
shadowhook_hook_sym_name_func shadowhook_hook_sym_name = nullptr;
shadowhook_get_errno_func shadowhook_get_errno = nullptr;
shadowhook_to_errmsg_func shadowhook_to_errmsg = nullptr;

static void (*origin_glCompressedTexSubImage2D)(long, long, long, long, long, long, long, long, const void* data);
static void my_glCompressedTexSubImage2D(long target, long level, long x, long y, long width, long height, long format, long imageSize, const void* data ) {
    LOGD("glCompressedTexSubImage2D: format=0x%lx, w=%ld, h=%ld, size=%ld", format, width, height, imageSize);
    origin_glCompressedTexSubImage2D(target, level, x, y, width, height, format, imageSize, data);
}

static void (*origin_glCompressedTexImage2D)(int, int, int, int, int, long, int, const void* data);
static void my_glCompressedTexImage2D(int target, int level, int internalformat, int width, int height, long border, int imageSize, const void* data ) {
    LOGD("glCompressedTexImage2D: format=0x%x, w=%d, h=%d, size=%d", internalformat, width, height, imageSize);
    origin_glCompressedTexImage2D(target, level, internalformat, width, height, border, imageSize, data);
}

static std::unordered_set<std::string> blacklist = {
        "68fa3c8bb44a88b9acc746a7cc42d57d.unity3d",
        "680644370ebb0d89232ca4993f1bd95d.unity3d",
        "b9029a61afe8de3f0438bfe2b90b9858.unity3d",
        "532349f1b103bfeb1453f1e5323d06fe.unity3d",
        "53edec228724f1a5b99f47007258c25e.unity3d",
        "8536a79bab433a687f8802fd8e1e13cd.unity3d"
};
static int count = 0;
static std::map<std::string, std::string> loaded_assets;
static std::unordered_set<std::string> loaded_assets_uri;

static void set_property(const char* name, const char* value) {
    __system_property_set(name, value);
}

static std::string get_property(const char* name) {
    auto value = std::array<char, PROP_VALUE_MAX>();
    __system_property_get(name, value.data());
    return {value.data() };
}

static void load_texture_safe(const char* path, const std::string& uri, const std::string& texture) {
    static int count = 0;
    static int sleep_seconds = 0;
    if (sleep_seconds == 0) {
        std::string prop = get_property("jkchess.dump_sleep");
        if (prop.empty()) {
            sleep_seconds = 1;
        } else {
            sleep_seconds = atoi(prop.c_str());
        }
    }
    load_asset(path, uri, texture.c_str(), 5000);
    count++;
    if (count % 100 == 0) {
        LOGD("sleep for %d seconds...", sleep_seconds);
        sleep(sleep_seconds);
    }
}

void load_all_textures(const char* path) {
    LOGD("load asset[%d]: %s", count++, path);
    std::string uri;
    std::vector<std::string> textures;
    get_asset_info(path, uri, textures, 10000);
    size_t total = textures.size();
    LOGD("uri: %s, total=%zu", uri.c_str(), total);
    size_t current = 0;
    for (auto &texture : textures) {
        current++;
        LOGD("load texture [%zu/%zu]: %s", current, total, texture.c_str());
        set_property("dumpasset.name", texture.c_str());
        load_texture_safe(path, uri, texture.c_str());
        int wait_count = 0;
        while (true) {
            wait_count++;
            usleep(1000);
            if (get_property("dumpasset.name.done") == texture || wait_count > 1500) { // 1.5 seconds
                set_property("dumpasset.name.done", "");
                break;
            }
        }
        if (get_property("dumpasset.stop") == "1") {
            set_property("dumpasset.stop", "");
            break;
        }
    }
    LOGD("load all texture done [%zu/%zu]: %s", current, total, uri.c_str());
}

void dump_asset(const char* path) {
    usleep(10000);
    LOGD("dump asset[%d]: %s", count++, path);
    std::string uri;
    std::vector<std::string> textures;
    get_asset_info(path, uri, textures, 10000);
    LOGD("uri: %s", uri.c_str());
    for (auto &texture : textures) {
        LOGD("texture: %s", texture.c_str());
        if (texture == "h_twistedfate_1_show") {
            LOGD("load texture %s", texture.c_str());
            load_texture_safe(path, uri, texture.c_str());
        }
    }
}

void get_hash_by_name(const char *filename, char* hash) {
    const char *dot = strrchr(filename, '.');

    size_t hash_length = dot - filename;

    if (hash_length > 0 && hash_length <= 128) {
        strncpy(hash, filename, hash_length);
        hash[hash_length] = '\0';  // 确保以null结尾
    } else {
        strcpy(hash, filename);
    }
}

bool exists(const char *path) {
    if (access(path, F_OK) == 0) {
        return true;
    }
    return false;
}

// 递归获取目录下所有文件名
void list_files_recursive(const char *base_path, int depth) {
    char path[1024];
    struct dirent *dp;
    DIR *dir = opendir(base_path);

    if (!dir) {
        LOGD("无法打开目录");
        return;
    }

    while ((dp = readdir(dir)) != NULL) {
        // 跳过 "." 和 ".."
        if (strcmp(dp->d_name, ".") == 0 || strcmp(dp->d_name, "..") == 0) {
            continue;
        }

        // 构建完整路径
        snprintf(path, sizeof(path), "%s/%s", base_path, dp->d_name);

        // 检查是否为目录
        struct stat statbuf;
        if (stat(path, &statbuf) == -1) {
            LOGD("stat 错误");
            continue;
        }

        if (S_ISDIR(statbuf.st_mode)) {
            // 递归处理子目录
            list_files_recursive(path, depth + 1);
        } else {
            // 输出文件名（不包含目录路径）
            LOGD("%s", dp->d_name);
            
            if (blacklist.find(dp->d_name) != blacklist.end()) {
                LOGD("skip blacklist: %s", dp->d_name);
                continue;
            }
            char exclude[256] = { 0 };
            sprintf(exclude, "%s%s", g_exclude_path.c_str(), dp->d_name);
            if (exists(exclude)) {
                LOGD("skip exclude: %s", dp->d_name);
                continue;
            }

            char hash[128] = { 0 };
            get_hash_by_name(dp->d_name, hash);
            if (loaded_assets.find(hash) != loaded_assets.end()) {
                LOGD("skip loaded: %s", dp->d_name);
                continue;
            }
            char asset_path[1024] = { 0 };
            sprintf(asset_path, "%s/%c%c/%s",
                    g_apk_asset_path.c_str(),
                    dp->d_name[0], dp->d_name[1], dp->d_name);

            //sprintf(asset_path, "/data/app/~~iDUJ3_95Z5oxcRkX3Vt_Lg==/com.tencent.jkchess-96MHX-4aZzh9ZijtR3JHfA==/base.apk!assets/AssetBundles/Android/%s",
            //        "cb/cb022288ca98895140bf217d87e5edc7.unity3d");
            dump_asset(asset_path);
        }
    }

    closedir(dir);
}

bool get_asset_path(const char* tkhash, char* path) {
    // 检查更新路径1
    sprintf(path, "%s/%c%c/%s.unity3d", g_update_asset_path1.c_str(), tkhash[0], tkhash[1], tkhash);
    if (exists(path)) {
        return true;
    }

    // 检查更新路径2
    sprintf(path, "%s/%c%c/%s.unity3d", g_update_asset_path2.c_str(), tkhash[0], tkhash[1], tkhash);
    if (exists(path)) {
        return true;
    }

    // 检查外部COS路径
    sprintf(path, "%s/%c%c/%s.cos", g_external_cos_path.c_str(), tkhash[0], tkhash[1], tkhash);
    if (exists(path)) {
        return true;
    }

    // 检查内部COS路径
    sprintf(path, "%s/%c%c/%s.cos", g_internal_cos_path.c_str(), tkhash[0], tkhash[1], tkhash);
    if (exists(path)) {
        return true;
    }

    // 检查APK中是否存在该asset
    char asset_in_apk[256];
    sprintf(asset_in_apk, "AssetBundles/Android/%c%c/%s.unity3d", tkhash[0], tkhash[1], tkhash);
    if (!g_apk_path.empty() && assetExistsInApk(g_apk_path.c_str(), asset_in_apk)) {
        sprintf(path, "%s/%c%c/%s.unity3d", g_apk_asset_path.c_str(), tkhash[0], tkhash[1], tkhash);
        return true;
    }

    return false;
}

void dump_by_uri(const char* uri) {
    if (loaded_assets_uri.find(uri) != loaded_assets_uri.end()) {
        LOGD("skip loaded: %s", uri);
        return;
    }
    loaded_assets_uri.insert(uri);
    
    char asset_path[256] = { 0 };
    auto tkhash = get_TKHash128(uri);
    LOGD("tkhash: %s", tkhash.c_str());
    if (get_asset_path(tkhash.c_str(), asset_path)) {
        LOGD("load all textures: %s", asset_path);
        load_all_textures(asset_path);
    } else {
        LOGD("asset not found: %s", uri);
    }
}

void log_hook_error() {
    int err_num = shadowhook_get_errno();
    const char *err_msg = shadowhook_to_errmsg(err_num);
    LOGD("hook error %d - %s", err_num, err_msg);
}

void dump_start(const char *game_data_dir) {
    // 初始化动态路径
    InitPaths(game_data_dir);
    
    char path[256] = { 0 };
    sprintf(path, "%s/dumpasset", game_data_dir);
    while (true) {
        LOGD("m2x waiting for %s ...", path);
        sleep(2);
        if (access(path, F_OK) == 0) {
            remove(path);
            break;
        }
    }
    void* handle = dlopen("/data/system/etc/mumu-configs/shared_libs/libandroidsh.so", RTLD_NOW);
    if (!handle) {
        LOGD("m2x load libandroidsh.so failed!");
        return;
    }
    shadowhook_init = (shadowhook_init_func)dlsym(handle, "shadowhook_init");
    shadowhook_hook_sym_name = (shadowhook_hook_sym_name_func)dlsym(handle, "shadowhook_hook_sym_name");
    shadowhook_get_errno = (shadowhook_get_errno_func)dlsym(handle, "shadowhook_get_errno");
    shadowhook_to_errmsg = (shadowhook_to_errmsg_func)dlsym(handle, "shadowhook_to_errmsg");
    LOGD("shadowhook_init: %p", shadowhook_init);
    LOGD("shadowhook_get_errno: %p", shadowhook_get_errno);
    LOGD("shadowhook_to_errmsg: %p", shadowhook_to_errmsg);
    LOGD("shadowhook_hook_sym_name: %p", shadowhook_hook_sym_name);
    shadowhook_init(0, 1);

//    shadowhook_hook_sym_name("/system/lib64/arm64/nb/libGLESv2.so", "glCompressedTexImage2D",
//                             (void *) my_glCompressedTexImage2D, (void **) &origin_glCompressedTexImage2D);
//    LOGD("glCompressedTexImage2D: %p", origin_glCompressedTexImage2D);
//    if (!origin_glCompressedTexImage2D) {
//        LOGD("failed to hook glCompressedTexImage2D!");
//        log_hook_error();
//        return;
//    }
//
//    shadowhook_hook_sym_name("/system/lib64/arm64/nb/libGLESv2.so", "glCompressedTexSubImage2D",
//                             (void *) my_glCompressedTexSubImage2D, (void **) &origin_glCompressedTexSubImage2D);
//    LOGD("glCompressedTexSubImage2D: %p", origin_glCompressedTexSubImage2D);
//    if (!origin_glCompressedTexSubImage2D) {
//        LOGD("failed to hook glCompressedTexSubImage2D!");
//        return;
//    }

    //void *handle = dlopen("libKtool.so", RTLD_NOW);
    handle = dlopen("/data/system/etc/mumu-configs/shared_libs/libKtool.so", RTLD_NOW);
    if (!handle) {
        LOGD("m2x load libKtool.so failed!");
        return;
    }


    get_asset_info = (get_asset_info_func)dlsym(handle, "get_AssetBundle_Texture2D_info");

    load_asset = (load_asset_func)dlsym(handle, "load_AssetBundle_Texture2D");
    get_loaded_assets = (get_loaded_assets_func) dlsym(handle, "loadedhase");
    get_TKHash128 = (get_TKHash128_func) dlsym(handle, "get_TKHash128");
    LOGD("get_AssetBundle_Texture2D_info:%p, load_AssetBundle_Texture2D %p, loadedhase %p, get_TKHash128: %p",
         get_asset_info, load_asset, get_loaded_assets, get_TKHash128);
    sleep(1);
    count = 0;
    loaded_assets = get_loaded_assets();
    LOGD("get_loaded_assets size: %zu", loaded_assets.size());
    for (auto &asset : loaded_assets) {
        loaded_assets_uri.insert(asset.second.c_str());
        LOGD("loaded: %s, %s", asset.first.c_str(), asset.second.c_str());
    }
    //auto tkhash = get_TKHash128("art_tft_raw/hero/hero_show/set1/h_twistedfate_1_show_high_res.unity3d");
    //LOGD("tkhash: %s", tkhash.c_str());
    //dump_asset("/data/app/~~iDUJ3_95Z5oxcRkX3Vt_Lg==/com.tencent.jkchess-96MHX-4aZzh9ZijtR3JHfA==/base.apk!assets/AssetBundles/Android/95/9501b19ffc319fa3ca4b552b09caff89.unity3d");
    //dump_asset("/sdcard/Android/data/com.tencent.jkchess/files/COSABResource/Android/a5/a5ba3ecaf91b6389d79d6eb6cb0e533d.cos");
    //list_files_recursive("/data/data/com.tencent.jkchess/Android", 0);
    //dump_by_uri("art_tft_raw/model_res/hero/set1/texture/h_lulu/lv1/materials/textures.unity3d");
    //dump_asset("/data/data/com.tencent.jkchess/files/COSABResource/Android/08/08907d46abe7d1424ef87589d7efd77d.cos");

    LOGD("m2x waiting for prop: jkchess.dump_uri ...");
    while (true) {
        usleep(100000);
        auto prop = get_property("jkchess.dump_uri");
        if (prop != "") {
            dump_by_uri(prop.c_str());
            set_property("jkchess.dump_uri", "");
        }
    }
}

void hack_start(const char *game_data_dir) {
    bool load = false;
    char path[256] = { 0 };
    sprintf(path, "%s/begindump", game_data_dir);
    while (true) {
        LOGD("m2x waiting for %s ...", path);
        sleep(2);
        if (access(path, F_OK) == 0) {
            remove(path);
            break;
        }
    }
    for (int i = 0; i < 10; i++) {
        void *handle = xdl_open("libil2cpp.so", 0);
        if (handle) {
            load = true;
            il2cpp_api_init(handle);
            il2cpp_dump(game_data_dir);
            break;
        } else {
            sleep(1);
        }
    }
    if (!load) {
        LOGI("libil2cpp.so not found in thread %d", gettid());
    }
}

std::string GetLibDir(JavaVM *vms) {
    JNIEnv *env = nullptr;
    jint result = vms->GetEnv(reinterpret_cast<void**>(&env), JNI_VERSION_1_6);
    if (result == JNI_EDETACHED) {
        if (vms->AttachCurrentThread(&env, nullptr) != JNI_OK) {
            LOGE("AttachCurrentThread failed");
            return {};
        }
    } else if (result != JNI_OK || env == nullptr) {
        LOGE("GetEnv failed: %d", result);
        return {};
    }
    jclass activity_thread_clz = env->FindClass("android/app/ActivityThread");
    if (activity_thread_clz != nullptr) {
        jmethodID currentApplicationId = env->GetStaticMethodID(activity_thread_clz,
                                                                "currentApplication",
                                                                "()Landroid/app/Application;");
        if (currentApplicationId) {
            jobject application = env->CallStaticObjectMethod(activity_thread_clz,
                                                              currentApplicationId);
            jclass application_clazz = env->GetObjectClass(application);
            if (application_clazz) {
                jmethodID get_application_info = env->GetMethodID(application_clazz,
                                                                  "getApplicationInfo",
                                                                  "()Landroid/content/pm/ApplicationInfo;");
                if (get_application_info) {
                    jobject application_info = env->CallObjectMethod(application,
                                                                     get_application_info);
                    jfieldID native_library_dir_id = env->GetFieldID(
                            env->GetObjectClass(application_info), "nativeLibraryDir",
                            "Ljava/lang/String;");
                    if (native_library_dir_id) {
                        auto native_library_dir_jstring = (jstring) env->GetObjectField(
                                application_info, native_library_dir_id);
                        auto path = env->GetStringUTFChars(native_library_dir_jstring, nullptr);
                        LOGI("lib dir %s", path);
                        std::string lib_dir(path);
                        env->ReleaseStringUTFChars(native_library_dir_jstring, path);
                        return lib_dir;
                    } else {
                        LOGE("nativeLibraryDir not found");
                    }
                } else {
                    LOGE("getApplicationInfo not found");
                }
            } else {
                LOGE("application class not found");
            }
        } else {
            LOGE("currentApplication not found");
        }
    } else {
        LOGE("ActivityThread not found");
    }
    return {};
}

static std::string GetNativeBridgeLibrary() {
    auto value = std::array<char, PROP_VALUE_MAX>();
    __system_property_get("ro.dalvik.vm.native.bridge", value.data());
    return {value.data()};
}

struct NativeBridgeCallbacks {
    uint32_t version;
    void *initialize;

    void *(*loadLibrary)(const char *libpath, int flag);

    void *(*getTrampoline)(void *handle, const char *name, const char *shorty, uint32_t len);

    void *isSupported;
    void *getAppEnv;
    void *isCompatibleWith;
    void *getSignalHandler;
    void *unloadLibrary;
    void *getError;
    void *isPathSupported;
    void *initAnonymousNamespace;
    void *createNamespace;
    void *linkNamespaces;

    void *(*loadLibraryExt)(const char *libpath, int flag, void *ns);
};

bool NativeBridgeLoad(const char *game_data_dir, int api_level, void *data, size_t length) {
    //TODO 等待houdini初始化
    sleep(5);

    auto libart = dlopen("libart.so", RTLD_NOW);
    auto JNI_GetCreatedJavaVMs = (jint (*)(JavaVM **, jsize, jsize *)) dlsym(libart,
                                                                             "JNI_GetCreatedJavaVMs");
    LOGI("JNI_GetCreatedJavaVMs %p", JNI_GetCreatedJavaVMs);
    JavaVM *vms_buf[1];
    JavaVM *vms;
    jsize num_vms;
    jint status = JNI_GetCreatedJavaVMs(vms_buf, 1, &num_vms);
    if (status == JNI_OK && num_vms > 0) {
        vms = vms_buf[0];
    } else {
        LOGE("GetCreatedJavaVMs error");
        return false;
    }

    auto lib_dir = GetLibDir(vms);
    if (lib_dir.empty()) {
        LOGE("GetLibDir error");
        return false;
    }
    if (lib_dir.find("/lib/x86") != std::string::npos) {
        LOGI("no need NativeBridge");
        munmap(data, length);
        return false;
    }

    auto nb = dlopen("libhoudini.so", RTLD_NOW);
    if (!nb) {
        auto native_bridge = GetNativeBridgeLibrary();
        LOGI("native bridge: %s", native_bridge.data());
        nb = dlopen(native_bridge.data(), RTLD_NOW);
    }
    if (nb) {
        LOGI("nb %p", nb);
        auto callbacks = (NativeBridgeCallbacks *) dlsym(nb, "NativeBridgeItf");
        if (callbacks) {
            LOGI("NativeBridgeLoadLibrary %p", callbacks->loadLibrary);
            LOGI("NativeBridgeLoadLibraryExt %p", callbacks->loadLibraryExt);
            LOGI("NativeBridgeGetTrampoline %p", callbacks->getTrampoline);

            int fd = syscall(__NR_memfd_create, "anon", MFD_CLOEXEC);
            ftruncate(fd, (off_t) length);
            void *mem = mmap(nullptr, length, PROT_WRITE, MAP_SHARED, fd, 0);
            memcpy(mem, data, length);
            munmap(mem, length);
            munmap(data, length);
            char path[PATH_MAX];
            snprintf(path, PATH_MAX, "/proc/self/fd/%d", fd);
            LOGI("arm path %s", path);

            void *arm_handle;
            if (api_level >= 26) {
                arm_handle = callbacks->loadLibraryExt(path, RTLD_NOW, (void *) 3);
            } else {
                arm_handle = callbacks->loadLibrary(path, RTLD_NOW);
            }
            if (arm_handle) {
                LOGI("arm handle %p", arm_handle);
                auto init = (void (*)(JavaVM *, void *)) callbacks->getTrampoline(arm_handle,
                                                                                  "JNI_OnLoad",
                                                                                  nullptr, 0);
                LOGI("JNI_OnLoad %p", init);
                init(vms, (void *) game_data_dir);
                return true;
            }
            close(fd);
        }
    }
    return false;
}

void hack_prepare(const char *game_data_dir, void *data, size_t length) {
    LOGI("hack thread: %d", gettid());
    int api_level = android_get_device_api_level();
    LOGI("api level: %d", api_level);

#if defined(__i386__) || defined(__x86_64__)
    if (!NativeBridgeLoad(game_data_dir, api_level, data, length)) {
#endif
        hack_start(game_data_dir);
#if defined(__i386__) || defined(__x86_64__)
    }
#endif
}

#if defined(__arm__) || defined(__aarch64__)

JNIEXPORT jint JNICALL JNI_OnLoad(JavaVM *vm, void *reserved) {
    auto game_data_dir = (const char *) reserved;
    std::thread hack_thread(hack_start, game_data_dir);
    hack_thread.detach();
    std::thread hack_thread2(dump_start, game_data_dir);
    hack_thread2.detach();
    return JNI_VERSION_1_6;
}

#endif