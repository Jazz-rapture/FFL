#pragma once
#include <string>
#include <vector>
#include <Windows.h>
#include <wincrypt.h>

#pragma comment(lib, "crypt32.lib")

namespace Core {

class HashUtils {
public:
    // 计算 SHA-1
    static std::string ComputeSHA1(const std::string& data) {
        HCRYPTPROV hProv = 0;
        HCRYPTHASH hHash = 0;
        BYTE hash[20];
        DWORD hashLen = 20;
        std::string result;

        if (!CryptAcquireContext(&hProv, nullptr, nullptr, PROV_RSA_FULL, CRYPT_VERIFYCONTEXT))
            return "";
        if (!CryptCreateHash(hProv, CALG_SHA1, 0, 0, &hHash)) {
            CryptReleaseContext(hProv, 0);
            return "";
        }
        if (!CryptHashData(hHash, (const BYTE*)data.c_str(), (DWORD)data.size(), 0)) {
            CryptDestroyHash(hHash);
            CryptReleaseContext(hProv, 0);
            return "";
        }
        if (!CryptGetHashParam(hHash, HP_HASHVAL, hash, &hashLen, 0)) {
            CryptDestroyHash(hHash);
            CryptReleaseContext(hProv, 0);
            return "";
        }

        char hex[41] = {};
        for (DWORD i = 0; i < hashLen; i++) {
            sprintf(hex + i * 2, "%02x", hash[i]);
        }
        result = hex;

        CryptDestroyHash(hHash);
        CryptReleaseContext(hProv, 0);
        return result;
    }

    // 计算文件 SHA-1
    static std::string ComputeFileSHA1(const std::wstring& filePath) {
        HANDLE hFile = CreateFileW(filePath.c_str(), GENERIC_READ, FILE_SHARE_READ,
                                    nullptr, OPEN_EXISTING, 0, nullptr);
        if (hFile == INVALID_HANDLE_VALUE) return "";

        HCRYPTPROV hProv = 0;
        HCRYPTHASH hHash = 0;
        CryptAcquireContext(&hProv, nullptr, nullptr, PROV_RSA_FULL, CRYPT_VERIFYCONTEXT);
        CryptCreateHash(hProv, CALG_SHA1, 0, 0, &hHash);

        BYTE buf[8192] = {0};
        DWORD bytesRead = 0;
        while (ReadFile(hFile, buf, sizeof(buf), &bytesRead, nullptr) && bytesRead > 0) {
            CryptHashData(hHash, buf, bytesRead, 0);
        }
        CloseHandle(hFile);

        BYTE hash[20];
        DWORD hashLen = 20;
        CryptGetHashParam(hHash, HP_HASHVAL, hash, &hashLen, 0);

        char hex[41] = {};
        for (DWORD i = 0; i < hashLen; i++) {
            sprintf(hex + i * 2, "%02x", hash[i]);
        }

        CryptDestroyHash(hHash);
        CryptReleaseContext(hProv, 0);
        return std::string(hex);
    }

    // 计算 MD5
    static std::string ComputeMD5(const std::string& data) {
        HCRYPTPROV hProv = 0;
        HCRYPTHASH hHash = 0;
        BYTE hash[16];
        DWORD hashLen = 16;

        if (!CryptAcquireContext(&hProv, nullptr, nullptr, PROV_RSA_FULL, CRYPT_VERIFYCONTEXT))
            return "";
        if (!CryptCreateHash(hProv, CALG_MD5, 0, 0, &hHash)) {
            CryptReleaseContext(hProv, 0);
            return "";
        }
        if (!CryptHashData(hHash, (const BYTE*)data.c_str(), (DWORD)data.size(), 0)) {
            CryptDestroyHash(hHash);
            CryptReleaseContext(hProv, 0);
            return "";
        }
        if (!CryptGetHashParam(hHash, HP_HASHVAL, hash, &hashLen, 0)) {
            CryptDestroyHash(hHash);
            CryptReleaseContext(hProv, 0);
            return "";
        }
        CryptDestroyHash(hHash);
        CryptReleaseContext(hProv, 0);

        std::string result;
        char hex[3] = {};
        for (DWORD i = 0; i < hashLen; i++) {
            sprintf(hex, "%02x", hash[i]);
            result += hex;
        }
        return result;
    }

    // 生成离线 UUID
    static std::string GenerateOfflineUUID(const std::string& username) {
        std::string input = "OfflinePlayer:" + username;
        std::string md5 = ComputeMD5(input);
        if (md5.empty()) return "";

        std::vector<BYTE> bytes(16);
        for (size_t i = 0; i < 16; i++) {
            std::string byteStr = md5.substr(i * 2, 2);
            bytes[i] = (BYTE)strtol(byteStr.c_str(), nullptr, 16);
        }

        // 设置版本和变体位
        bytes[6] = (bytes[6] & 0x0F) | 0x30;  // 版本 3
        bytes[8] = (bytes[8] & 0x3F) | 0x80;  // 变体 2

        char uuid[37];
        snprintf(uuid, sizeof(uuid),
            "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x",
            bytes[0], bytes[1], bytes[2], bytes[3],
            bytes[4], bytes[5],
            bytes[6], bytes[7],
            bytes[8], bytes[9],
            bytes[10], bytes[11], bytes[12], bytes[13], bytes[14], bytes[15]);

        return std::string(uuid);
    }
};

} // namespace Core
