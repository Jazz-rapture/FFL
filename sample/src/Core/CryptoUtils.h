#pragma once
#include <string>
#include <vector>
#include <cstring>
#include <Windows.h>
#include <wincrypt.h>

#pragma comment(lib, "crypt32.lib")

namespace Core {

// 账号令牌的本地保护。
// DPAPI 用当前 Windows 用户的凭证加密，密文写进 config.json 后，
// 即使文件被拷走、同步到网盘或被别的程序读到，也无法直接拿去登录。
// 代价是换 Windows 用户 / 换机器后解密会失败——那种情况本来也必须重新登录。
class CryptoUtils {
public:
    // 固定熵：DPAPI 的 optionalEntropy。同一台机器上的其它程序即使也以
    // 当前用户身份运行，不知道这段熵同样解不开。改这个值会让已存储的令牌失效。
    static const char* Entropy() { return "TauriCPP/account-token/v1"; }

    // 加密后转 Base64（DPAPI 输出是二进制，放不进 JSON 字符串）
    static std::string ProtectToBase64(const std::string& plain) {
        if (plain.empty()) return "";

        DATA_BLOB in{};
        in.pbData = (BYTE*)plain.data();
        in.cbData = (DWORD)plain.size();

        std::string entropyText = Entropy();
        DATA_BLOB entropy{};
        entropy.pbData = (BYTE*)entropyText.data();
        entropy.cbData = (DWORD)entropyText.size();

        DATA_BLOB out{};
        BOOL ok = CryptProtectData(&in, L"TauriCPP account token", &entropy, nullptr, nullptr,
                                   CRYPTPROTECT_UI_FORBIDDEN, &out);
        if (!ok) return "";

        std::string encoded = Base64Encode(out.pbData, out.cbData);
        LocalFree(out.pbData);
        return encoded;
    }

    // Base64 -> 解密；失败（换用户 / 换机器 / 内容被改）返回空串
    static std::string UnprotectFromBase64(const std::string& encoded) {
        std::vector<BYTE> raw = Base64Decode(encoded);
        if (raw.empty()) return "";

        DATA_BLOB in{};
        in.pbData = raw.data();
        in.cbData = (DWORD)raw.size();

        std::string entropyText = Entropy();
        DATA_BLOB entropy{};
        entropy.pbData = (BYTE*)entropyText.data();
        entropy.cbData = (DWORD)entropyText.size();

        DATA_BLOB out{};
        BOOL ok = CryptUnprotectData(&in, nullptr, &entropy, nullptr, nullptr,
                                     CRYPTPROTECT_UI_FORBIDDEN, &out);
        if (!ok) return "";

        std::string plain((const char*)out.pbData, out.cbData);
        LocalFree(out.pbData);
        return plain;
    }

    static std::string Base64Encode(const BYTE* data, size_t len) {
        static const char* table =
            "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
        std::string out;
        out.reserve(((len + 2) / 3) * 4);

        size_t i = 0;
        while (i + 2 < len) {
            unsigned v = ((unsigned)data[i] << 16) | ((unsigned)data[i + 1] << 8) | data[i + 2];
            out += table[(v >> 18) & 63];
            out += table[(v >> 12) & 63];
            out += table[(v >> 6) & 63];
            out += table[v & 63];
            i += 3;
        }

        const size_t rest = len - i;
        if (rest == 1) {
            unsigned v = (unsigned)data[i] << 16;
            out += table[(v >> 18) & 63];
            out += table[(v >> 12) & 63];
            out += "==";
        } else if (rest == 2) {
            unsigned v = ((unsigned)data[i] << 16) | ((unsigned)data[i + 1] << 8);
            out += table[(v >> 18) & 63];
            out += table[(v >> 12) & 63];
            out += table[(v >> 6) & 63];
            out += '=';
        }
        return out;
    }

    // '=' 与换行空白一律跳过，因此也能解带换行的 PEM 风格内容
    static std::vector<BYTE> Base64Decode(const std::string& text) {
        std::vector<BYTE> out;
        out.reserve(text.size() / 4 * 3);

        int buf = 0;
        int bits = 0;
        for (char c : text) {
            int v = -1;
            if (c >= 'A' && c <= 'Z') v = c - 'A';
            else if (c >= 'a' && c <= 'z') v = c - 'a' + 26;
            else if (c >= '0' && c <= '9') v = c - '0' + 52;
            else if (c == '+') v = 62;
            else if (c == '/') v = 63;
            else continue;

            buf = (buf << 6) | v;
            bits += 6;
            if (bits >= 8) {
                bits -= 8;
                out.push_back((BYTE)((buf >> bits) & 0xFF));
            }
        }
        return out;
    }
};

} // namespace Core
