// ============================================================================
//  luogu_solution_checker.cpp
//  批量爬取洛谷（luogu.com.cn）题目，判断「是否可以提交题解」。
//
//  判定原理（已实测验证）
//  ----------------------
//  洛谷把服务端渲染数据塞进每个页面的
//      <script id="lentille-context" type="application/json">{...}</script>
//  题目页：  data.problem.acceptSolution
//  题解页：  data.acceptSolution          （两个字段结论一致）
//
//  acceptSolution 是服务端按「当前访问账号」实时算出来的布尔值：
//      true  -> 右侧会渲染「提交题解」按钮 -> 可以投稿
//      false -> 右侧渲染「本题目已不接受新题解提交」
//               （题解数量已足够多 / 题目过于简单 / 处于月赛保护期）
//
//  实测样例（账号 qzh_2015）：
//      P1000/P1044/P1048/P2249/P1001/P1008 ... acceptSolution = false
//      P9093 [PA 2020] Punkty rankingowe      acceptSolution = true
//  所以该字段确实会返回 true，并不是恒定 false。
//  注意：false 并不意味着「你已经提交过题解」——P1000 已 AC 时仍为 false。
//
//  验证已登录 / 未登录
//  ------------------
//  题目页 JSON 顶层 'user' 字段：
//      登录 -> 完整的用户对象（uid / name / color ...）
//      匿名 -> null
//  这是最可靠的登录态探针（题解页匿名访问会直接 401）。
//
//  编译
//  ----
//   Windows / MinGW-w64（零第三方依赖，用系统 WinHTTP）：
//       g++ -std=c++17 -O2 luogu_solution_checker.cpp -o luogu_check.exe -lwinhttp
//   Linux / macOS（libcurl）：
//       g++ -std=c++17 -O2 luogu_solution_checker.cpp -o luogu_check -lcurl
//
//  用法
//  ----
//     luogu_check.exe P1000 P1044 P9093
//     luogu_check.exe --cookie cookie.txt P1000 P9093
//     luogu_check.exe -f ids.txt --csv result.csv --delay-min 3000 --delay-max 10000
//
//     # 按难度自动枚举题目（不需要自己准备题号）
//     luogu_check.exe --difficulty 入门 --cookie cookie.txt --limit 5
//     luogu_check.exe --difficulty 普及-,普及/提高- --pages 1-3 --csv out.csv
//     luogu_check.exe --difficulty 1,2 --offset 0 --limit 50 --delay 3000
//     luogu_check.exe --list-difficulty              # 只看各难度题目数量
//
//  说明：每抓完一题自动随机等待 3~10 秒，模拟真人节奏，降低被风控的概率。
//  不登录时 acceptSolution 恒为 false，无法判断，程序会明确标注「未知（需登录）」。
//
//  难度枚举的原理
//  --------------
//  洛谷官方的题目列表接口：
//      https://www.luogu.com.cn/problem/list?difficulty=<值>&page=<页>&_contentOnly=1
//  请求头带 x-luogu-type: content-only，返回的是 HTML，数据依旧在 lentille-context 里，
//  结构为 data.problems.{perPage,count,result[]}。
//      difficulty 取 1..7 时按对应难度筛选；
//      difficulty 取 0（或干脆不传）时不限难度，全站约 1.8 万题。
//  本程序据此实现「--difficulty 入门,普及- --limit 50」这样的用法：
//  先枚举题目列表拿到题号，再逐题抓详情判断是否能提交题解。
//
//  Cookie 获取：浏览器登录洛谷 -> F12 -> Network -> 任意请求 -> 复制 Cookie 请求头，
//  存成一行 `_uid=...; __client_id=...` 放进文件即可。
// ============================================================================

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iostream>
#include <random>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <shellapi.h>  // CommandLineToArgvW
#include <winhttp.h>
#else
#include <curl/curl.h>
#endif

// ---------------------------------------------------------------------------
// 最小 JSON 解析器（够解析洛谷的 lentille-context）
// ---------------------------------------------------------------------------
struct JVal {
    enum Type { NUL, BOOL, NUM, STR, ARR, OBJ } type = NUL;
    bool b = false;
    double num = 0;
    std::string str;
    std::vector<JVal> arr;
    std::vector<std::pair<std::string, JVal>> obj;

    const JVal* get(const std::string& key) const {
        if (type != OBJ) return nullptr;
        for (const auto& kv : obj)
            if (kv.first == key) return &kv.second;
        return nullptr;
    }
    bool has(const std::string& key) const { return get(key) != nullptr; }
    std::string asStr() const { return type == STR ? str : std::string(); }
    long long asInt() const {
        if (type == NUM) return static_cast<long long>(num);
        return 0;
    }
    bool asBool() const { return type == BOOL ? b : (type == NUM ? num != 0 : false); }
};

class JsonParser {
public:
    // 按值保存一份：调用方大量传入 html.substr(...) 这样的临时对象，
    // 若存 const std::string& 会变成悬空引用（UB），表现为"同样的数据有时能解析有时不能"。
    explicit JsonParser(std::string s) : s_(std::move(s)), i_(0) {}

    bool parse(JVal& out) {
        skipWs();
        if (!parseValue(out)) return false;
        skipWs();
        return true;
    }

private:
    std::string s_;
    size_t i_;

    void skipWs() {
        while (i_ < s_.size() && (s_[i_] == ' ' || s_[i_] == '\t' || s_[i_] == '\n' || s_[i_] == '\r'))
            ++i_;
    }
    bool lit(const char* w) {
        size_t n = std::strlen(w);
        if (s_.compare(i_, n, w) != 0) return false;
        i_ += n;
        return true;
    }

    static void putUtf8(std::string& o, unsigned cp) {
        if (cp < 0x80) {
            o += static_cast<char>(cp);
        } else if (cp < 0x800) {
            o += static_cast<char>(0xC0 | (cp >> 6));
            o += static_cast<char>(0x80 | (cp & 0x3F));
        } else if (cp < 0x10000) {
            o += static_cast<char>(0xE0 | (cp >> 12));
            o += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
            o += static_cast<char>(0x80 | (cp & 0x3F));
        } else {
            o += static_cast<char>(0xF0 | (cp >> 18));
            o += static_cast<char>(0x80 | ((cp >> 12) & 0x3F));
            o += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
            o += static_cast<char>(0x80 | (cp & 0x3F));
        }
    }

    bool parseString(std::string& out) {
        if (i_ >= s_.size() || s_[i_] != '"') return false;
        ++i_;
        out.clear();
        while (i_ < s_.size()) {
            char c = s_[i_++];
            if (c == '"') return true;
            if (c != '\\') {
                out += c;
                continue;
            }
            if (i_ >= s_.size()) return false;
            char e = s_[i_++];
            switch (e) {
                case '"': out += '"'; break;
                case '\\': out += '\\'; break;
                case '/': out += '/'; break;
                case 'b': out += '\b'; break;
                case 'f': out += '\f'; break;
                case 'n': out += '\n'; break;
                case 'r': out += '\r'; break;
                case 't': out += '\t'; break;
                case 'u': {
                    if (i_ + 4 > s_.size()) return false;
                    unsigned cp = static_cast<unsigned>(std::strtoul(s_.substr(i_, 4).c_str(), nullptr, 16));
                    i_ += 4;
                    if (cp >= 0xD800 && cp <= 0xDBFF && i_ + 6 <= s_.size() && s_[i_] == '\\' &&
                        s_[i_ + 1] == 'u') {
                        unsigned lo =
                            static_cast<unsigned>(std::strtoul(s_.substr(i_ + 2, 4).c_str(), nullptr, 16));
                        if (lo >= 0xDC00 && lo <= 0xDFFF) {
                            cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                            i_ += 6;
                        }
                    }
                    putUtf8(out, cp);
                    break;
                }
                default: return false;
            }
        }
        return false;
    }

    bool parseValue(JVal& v) {
        skipWs();
        if (i_ >= s_.size()) return false;
        char c = s_[i_];
        if (c == '{') {
            ++i_;
            v.type = JVal::OBJ;
            skipWs();
            if (i_ < s_.size() && s_[i_] == '}') {
                ++i_;
                return true;
            }
            while (true) {
                skipWs();
                std::string key;
                if (!parseString(key)) return false;
                skipWs();
                if (i_ >= s_.size() || s_[i_] != ':') return false;
                ++i_;
                JVal child;
                if (!parseValue(child)) return false;
                v.obj.emplace_back(std::move(key), std::move(child));
                skipWs();
                if (i_ >= s_.size()) return false;
                if (s_[i_] == ',') {
                    ++i_;
                    continue;
                }
                if (s_[i_] == '}') {
                    ++i_;
                    return true;
                }
                return false;
            }
        }
        if (c == '[') {
            ++i_;
            v.type = JVal::ARR;
            skipWs();
            if (i_ < s_.size() && s_[i_] == ']') {
                ++i_;
                return true;
            }
            while (true) {
                JVal child;
                if (!parseValue(child)) return false;
                v.arr.push_back(std::move(child));
                skipWs();
                if (i_ >= s_.size()) return false;
                if (s_[i_] == ',') {
                    ++i_;
                    continue;
                }
                if (s_[i_] == ']') {
                    ++i_;
                    return true;
                }
                return false;
            }
        }
        if (c == '"') {
            v.type = JVal::STR;
            return parseString(v.str);
        }
        if (lit("true")) {
            v.type = JVal::BOOL;
            v.b = true;
            return true;
        }
        if (lit("false")) {
            v.type = JVal::BOOL;
            v.b = false;
            return true;
        }
        if (lit("null")) {
            v.type = JVal::NUL;
            return true;
        }
        // 严格按 JSON 数字文法扫描：-? int frac? exp?
        // 注意：不能把 ',' '}' ']' 之后的内容吞进来，也不允许 '+' 起头，
        // 否则 "status":200,... 这类后面紧跟分隔符的数字会把整段结构吃掉。
        size_t start = i_;
        if (i_ < s_.size() && s_[i_] == '-') ++i_;
        size_t digits = i_;
        while (i_ < s_.size() && std::isdigit(static_cast<unsigned char>(s_[i_]))) ++i_;
        if (i_ == digits) {  // 没有整数部分，不是数字
            i_ = start;
            return false;
        }
        if (i_ < s_.size() && s_[i_] == '.') {
            ++i_;
            size_t frac = i_;
            while (i_ < s_.size() && std::isdigit(static_cast<unsigned char>(s_[i_]))) ++i_;
            if (i_ == frac) return false;
        }
        if (i_ < s_.size() && (s_[i_] == 'e' || s_[i_] == 'E')) {
            ++i_;
            if (i_ < s_.size() && (s_[i_] == '+' || s_[i_] == '-')) ++i_;
            size_t expd = i_;
            while (i_ < s_.size() && std::isdigit(static_cast<unsigned char>(s_[i_]))) ++i_;
            if (i_ == expd) return false;
        }
        v.type = JVal::NUM;
        v.num = std::strtod(s_.substr(start, i_ - start).c_str(), nullptr);
        return true;
    }
};

// ---------------------------------------------------------------------------
// HTTP 客户端
// ---------------------------------------------------------------------------
struct HttpResponse {
    long status = 0;
    std::string body;
    std::string err;
    bool ok() const { return (status == 200 || status == 401) && !body.empty() && err.empty(); }
};

static const char* kUserAgent =
    "Mozilla/5.0 (Windows NT 10.0; Win64; x64; rv:160.0) Gecko/20100101 Firefox/160.0";

#ifdef _WIN32
static std::wstring u2w(const std::string& s) {
    if (s.empty()) return std::wstring();
    int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
    std::wstring w(static_cast<size_t>(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), &w[0], n);
    return w;
}

// WinHTTP 自带 Cookie 管理并自动跟随 3xx，正好兜住洛谷的 C3VK 反爬跳转。
static HttpResponse httpGet(const std::string& url, const std::string& cookie, int timeoutSec) {
    HttpResponse r;
    std::string u = url;
    bool https = (u.rfind("https://", 0) == 0);
    size_t sp = u.find("://");
    if (sp == std::string::npos) {
        r.err = "bad url";
        return r;
    }
    std::string rest = u.substr(sp + 3);
    size_t slash = rest.find('/');
    std::string hostport = (slash == std::string::npos) ? rest : rest.substr(0, slash);
    std::string path = (slash == std::string::npos) ? "/" : rest.substr(slash);
    unsigned short port = https ? 443 : 80;
    size_t colon = hostport.find(':');
    if (colon != std::string::npos) {
        port = static_cast<unsigned short>(std::atoi(hostport.c_str() + colon + 1));
        hostport = hostport.substr(0, colon);
    }

    HINTERNET session = WinHttpOpen(u2w(kUserAgent).c_str(), WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
                                    WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!session) {
        r.err = "WinHttpOpen 失败";
        return r;
    }
    int ms = timeoutSec * 1000;
    WinHttpSetTimeouts(session, ms, ms, ms, ms);

    HINTERNET conn = WinHttpConnect(session, u2w(hostport).c_str(), port, 0);
    if (!conn) {
        WinHttpCloseHandle(session);
        r.err = "WinHttpConnect 失败";
        return r;
    }
    HINTERNET req = WinHttpOpenRequest(conn, L"GET", u2w(path).c_str(), nullptr, WINHTTP_NO_REFERER,
                                       WINHTTP_DEFAULT_ACCEPT_TYPES, https ? WINHTTP_FLAG_SECURE : 0);
    if (!req) {
        WinHttpCloseHandle(conn);
        WinHttpCloseHandle(session);
        r.err = "WinHttpOpenRequest 失败";
        return r;
    }

    // 题目列表接口靠这个头走轻量渲染；加上它还能让洛谷直接返回完整 HTML。
    std::wstring headers = L"x-luogu-type: content-only\r\n";
    if (!cookie.empty()) headers += L"Cookie: " + u2w(cookie) + L"\r\n";

    BOOL sent = WinHttpSendRequest(req, headers.c_str(), static_cast<DWORD>(-1L), WINHTTP_NO_REQUEST_DATA, 0, 0, 0);
    if (!sent) {
        r.err = "WinHttpSendRequest 失败 (" + std::to_string(GetLastError()) + ")";
    } else if (!WinHttpReceiveResponse(req, nullptr)) {
        r.err = "WinHttpReceiveResponse 失败 (" + std::to_string(GetLastError()) + ")";
    } else {
        DWORD status = 0, slen = sizeof(status);
        WinHttpQueryHeaders(req, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                            WINHTTP_HEADER_NAME_BY_INDEX, &status, &slen, WINHTTP_NO_HEADER_INDEX);
        r.status = static_cast<long>(status);
        for (;;) {
            DWORD avail = 0;
            if (!WinHttpQueryDataAvailable(req, &avail) || avail == 0) break;
            std::string chunk(avail, '\0');
            DWORD got = 0;
            if (!WinHttpReadData(req, &chunk[0], avail, &got) || got == 0) break;
            chunk.resize(got);
            r.body += chunk;
        }
        if (r.status != 200 && r.status != 401) r.err = "HTTP " + std::to_string(r.status);
    }

    WinHttpCloseHandle(req);
    WinHttpCloseHandle(conn);
    WinHttpCloseHandle(session);
    return r;
}

#else  // ------------------------------ libcurl ------------------------------

static size_t writeCb(char* ptr, size_t size, size_t nmemb, void* userdata) {
    auto* out = static_cast<std::string*>(userdata);
    out->append(ptr, size * nmemb);
    return size * nmemb;
}

static HttpResponse httpGet(const std::string& url, const std::string& cookie, int timeoutSec) {
    HttpResponse r;
    static bool inited = false;
    if (!inited) {
        curl_global_init(CURL_GLOBAL_DEFAULT);
        inited = true;
    }

    CURL* h = curl_easy_init();
    if (!h) {
        r.err = "curl_easy_init 失败";
        return r;
    }
    struct curl_slist* hdrs = nullptr;
    hdrs = curl_slist_append(hdrs, "x-luogu-type: content-only");
    hdrs = curl_slist_append(hdrs, "Accept: text/html,application/json;q=0.9,*/*;q=0.8");
    hdrs = curl_slist_append(hdrs, "Accept-Language: zh-CN,zh;q=0.9");
    if (!cookie.empty()) hdrs = curl_slist_append(hdrs, ("Cookie: " + cookie).c_str());

    curl_easy_setopt(h, CURLOPT_URL, url.c_str());
    curl_easy_setopt(h, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(h, CURLOPT_MAXREDIRS, 20L);
    curl_easy_setopt(h, CURLOPT_COOKIEFILE, "");  // 开启 Cookie 引擎，自动保存 302 下发的 C3VK
    curl_easy_setopt(h, CURLOPT_USERAGENT, kUserAgent);
    curl_easy_setopt(h, CURLOPT_HTTPHEADER, hdrs);
    curl_easy_setopt(h, CURLOPT_WRITEFUNCTION, writeCb);
    curl_easy_setopt(h, CURLOPT_WRITEDATA, &r.body);
    curl_easy_setopt(h, CURLOPT_TIMEOUT, static_cast<long>(timeoutSec));
    curl_easy_setopt(h, CURLOPT_CONNECTTIMEOUT, static_cast<long>(timeoutSec));
    curl_easy_setopt(h, CURLOPT_ACCEPT_ENCODING, "");  // 自动解压

    CURLcode rc = curl_easy_perform(h);
    if (rc != CURLE_OK) {
        r.err = curl_easy_strerror(rc);
    } else {
        long code = 0;
        curl_easy_getinfo(h, CURLINFO_RESPONSE_CODE, &code);
        r.status = code;
        if (code != 200 && code != 401) r.err = "HTTP " + std::to_string(code);
    }
    curl_slist_free_all(hdrs);
    curl_easy_cleanup(h);
    return r;
}
#endif

// ---------------------------------------------------------------------------
// 按 UTF-8 路径打开文件
// ---------------------------------------------------------------------------
// Windows 上 std::ifstream/std::ofstream 的窄字符路径走 ANSI 代码页（本机为 GBK），
// 传 UTF-8 中文路径会被当作 GBK 解析，结果文件名变成乱码（如「结果.csv」写成「缁撴灉.csv」）。
// 这里统一用 Win32 的 _wfopen 打开 FILE*，再交给 stdio_filebuf，保证中文路径可用。
// （MinGW 8.1 的 std::filesystem 与 fstream::open(path) 配合有缺陷，故不走 filesystem。）
#ifdef _WIN32
static std::wstring pathW(const std::string& p) {
    if (p.empty()) return std::wstring();
    int n = MultiByteToWideChar(CP_UTF8, 0, p.data(), static_cast<int>(p.size()), nullptr, 0);
    std::wstring w(static_cast<size_t>(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, p.data(), static_cast<int>(p.size()), &w[0], n);
    return w;
}

// 只负责把文件内容读成字符串（题号文件、Cookie 文件都走这里）
static bool readWholeFile(const std::string& p, std::string& out) {
    FILE* f = _wfopen(pathW(p).c_str(), L"rb");
    if (!f) return false;
    out.clear();
    char buf[65536];
    size_t n;
    while ((n = std::fread(buf, 1, sizeof(buf), f)) > 0) out.append(buf, n);
    std::fclose(f);
    return true;
}

static bool writeWholeFile(const std::string& p, const std::string& data) {
    FILE* f = _wfopen(pathW(p).c_str(), L"wb");
    if (!f) return false;
    size_t n = data.empty() ? 0 : std::fwrite(data.data(), 1, data.size(), f);
    std::fclose(f);
    return n == data.size();
}
#else
static bool readWholeFile(const std::string& p, std::string& out) {
    std::ifstream fin(p, std::ios::binary);
    if (!fin) return false;
    std::ostringstream ss;
    ss << fin.rdbuf();
    out = ss.str();
    return true;
}
static bool writeWholeFile(const std::string& p, const std::string& data) {
    std::ofstream f(p, std::ios::binary);
    if (!f) return false;
    f.write(data.data(), static_cast<std::streamsize>(data.size()));
    return static_cast<bool>(f);
}
#endif

// ---------------------------------------------------------------------------
// 业务逻辑
// ---------------------------------------------------------------------------
static const char* kDiffName[] = {"尚无评定", "入门",   "普及-",     "普及/提高-", "普及+/提高",
                                  "提高+/省选-", "省选/NOI-", "NOI/NOI+/CTSC"};

static std::string diffName(int d) {
    return (d >= 0 && d <= 7) ? kDiffName[d] : "?";
}

// ---------------- 难度筛选常量 ----------------
// 和洛谷题目列表接口的 difficulty 参数取值一致：1..7 为真实难度，
// 0 表示「不限难度」（接口把 difficulty=0 当空筛选，返回全站题目）。
static const int kAnyDifficulty = 0;
static const int kMaxDifficulty = 7;

struct ProblemReport {
    std::string pid;
    std::string name = "-";
    std::string type = "P";
    int difficulty = -1;
    bool hasAcceptSolution = false;  // 字段本身是否存在
    bool acceptSolution = false;
    long long submit = 0, accepted = 0;
    long httpStatus = 0;
    std::string verdict;  // 结论
    std::string detail;   // 说明
};

// 从 HTML 里抠出 <script id="lentille-context" type="application/json">...</script>。
// 注意属性顺序可能变（例如 <script type="application/json" id="lentille-context">），
// 所以先定位 "lentille-context"，再以它为中心向前找 '<'、向后找 '>'，不依赖属性顺序。
static bool extractLentilleContext(const std::string& html, JVal& root) {
    // 从第一个 "lentille-context" 开始尝试；失败就继续找下一个（页面里可能还有其它引用）
    size_t from = 0;
    while (true) {
        size_t key = html.find("lentille-context", from);
        if (key == std::string::npos) return false;
        from = key + 1;

        size_t lt = html.rfind('<', key);  // 标签开头
        if (lt == std::string::npos) continue;
        size_t gt = html.find('>', key);  // 标签结尾
        if (gt == std::string::npos) continue;
        // 只接受真正承载 JSON 的 script 标签
        std::string tag = html.substr(lt, gt - lt + 1);
        if (tag.find("script") == std::string::npos) continue;
        if (tag.find("json") == std::string::npos) continue;

        size_t begin = gt + 1;
        size_t end = html.find("</script>", begin);
        if (end == std::string::npos) continue;
        JsonParser jp(html.substr(begin, end - begin));
        JVal v;
        if (jp.parse(v)) {
            root = std::move(v);
            return true;
        }
    }
}

static std::string trim(std::string s) {
    while (!s.empty() && (s.back() == '\r' || s.back() == '\n' || s.back() == ' ' || s.back() == '\t' ||
                          s.back() == ';'))
        s.pop_back();
    size_t i = 0;
    while (i < s.size() && (s[i] == ' ' || s[i] == '\t')) ++i;
    return s.substr(i);
}

// Cookie 参数：既可以是 "k=v; k2=v2" 原始串，也可以是文件路径
// （普通文本文件，或浏览器导出的 Netscape cookies.txt）
static std::string loadCookie(const std::string& arg) {
    std::string content;
    if (!readWholeFile(arg, content)) return trim(arg);

    std::istringstream fin(content);
    std::string line, cookie;
    while (std::getline(fin, line)) {
        if (!line.empty() && line[0] == '#') continue;  // 同时跳过注释和 Netscape 的 "#HttpOnly_" 前缀行
        line = trim(line);
        if (line.empty()) continue;

        std::vector<std::string> f;
        std::string cur;
        for (char c : line) {
            if (c == '\t') {
                f.push_back(cur);
                cur.clear();
            } else {
                cur += c;
            }
        }
        f.push_back(cur);

        std::string kv;
        if (f.size() >= 7) {
            kv = trim(f[5]) + "=" + trim(f[6]);  // Netscape: name / value
        } else {
            kv = line;
        }
        if (kv.empty() || kv.find('=') == std::string::npos) continue;
        if (!cookie.empty()) cookie += "; ";
        cookie += kv;
    }
    return cookie;
}

// 登录态：题目页 JSON 顶层 "user" 为对象即已登录，为 null 即匿名。
enum class LoginState { Anonymous, Verified, Invalid, Unknown };

struct LoginInfo {
    LoginState state = LoginState::Unknown;
    long long uid = 0;
    std::string name;
};

// 洛谷的反爬：首次请求 302 + Set-Cookie: C3VK=...，跳转回来后才是真正的页面。
// 偶发情况下 302 之后的响应仍然不是完整页面，所以这里再包一层「拿到可用页面为止」的重试。
static HttpResponse fetchPage(const std::string& url, const std::string& cookie, int timeoutSec,
                              std::string* reason = nullptr) {
    HttpResponse last;
    for (int attempt = 0; attempt < 3; ++attempt) {
        last = httpGet(url, cookie, timeoutSec);
        if (!last.ok()) {
            if (reason) *reason = last.err.empty() ? "空响应" : last.err;
            if (last.status >= 400 && last.status < 500) return last;  // 4xx 不重试
        } else {
            JVal root;
            if (extractLentilleContext(last.body, root)) return last;
            if (reason) *reason = "页面里没有 lentille-context（可能题号不存在，或被反爬拦截）";
        }
        if (attempt < 2) std::this_thread::sleep_for(std::chrono::milliseconds(1200 + attempt * 1800));
    }
    return last;
}

static LoginInfo probeLogin(const std::string& cookie, int timeoutSec) {
    LoginInfo info;
    if (cookie.empty()) {
        info.state = LoginState::Anonymous;
        return info;
    }
    std::string url = "https://www.luogu.com.cn/problem/P1000";
    HttpResponse r = fetchPage(url, cookie, timeoutSec);
    if (!r.ok()) {
        info.state = LoginState::Unknown;
        return info;
    }
    JVal root;
    if (!extractLentilleContext(r.body, root)) {
        info.state = LoginState::Unknown;
        return info;
    }
    const JVal* user = root.get("user");
    if (user && user->type == JVal::OBJ) {
        info.state = LoginState::Verified;
        if (const JVal* u = user->get("uid")) info.uid = u->asInt();
        if (const JVal* n = user->get("name")) info.name = n->asStr();
        if (info.uid == 0) info.state = LoginState::Invalid;
    } else if (user && user->type == JVal::NUL) {
        info.state = cookie.empty() ? LoginState::Anonymous : LoginState::Invalid;
    } else {
        info.state = LoginState::Unknown;
    }
    return info;
}

// 抓题目页，优先读 data.problem.acceptSolution；题目页缺这个字段时退回到题解页的 data.acceptSolution。
static ProblemReport checkProblem(const std::string& pid, const std::string& cookie, int timeoutSec,
                                  LoginState login) {
    ProblemReport rep;
    rep.pid = pid;

    std::string url = "https://www.luogu.com.cn/problem/" + pid;
    std::string reason;
    HttpResponse r = fetchPage(url, cookie, timeoutSec, &reason);
    rep.httpStatus = r.status;
    if (!r.ok()) {
        rep.verdict = "抓取失败";
        rep.detail = r.err.empty() ? "空响应" : r.err;
        return rep;
    }

    JVal root;
    if (!extractLentilleContext(r.body, root)) {
        rep.verdict = "抓取失败";
        rep.detail = reason.empty() ? "页面里没有 lentille-context（可能题号不存在，或被反爬拦截）" : reason;
        return rep;
    }

    const JVal* data = root.get("data");
    const JVal* problem = data ? data->get("problem") : nullptr;
    if (!problem) {
        const JVal* errCode = root.get("errorCode");
        rep.verdict = "抓取失败";
        rep.detail = errCode ? ("接口报错 errorCode=" + std::to_string(errCode->asInt()))
                             : "JSON 结构异常（没有 data.problem）";
        return rep;
    }

    if (const JVal* v = problem->get("pid")) rep.pid = v->asStr();
    if (const JVal* v = problem->get("name")) rep.name = v->asStr();
    if (const JVal* v = problem->get("type")) rep.type = v->asStr();
    if (const JVal* v = problem->get("difficulty")) rep.difficulty = static_cast<int>(v->asInt());
    if (const JVal* v = problem->get("totalSubmit")) rep.submit = v->asInt();
    if (const JVal* v = problem->get("totalAccepted")) rep.accepted = v->asInt();

    if (const JVal* v = problem->get("acceptSolution")) {
        rep.hasAcceptSolution = true;
        rep.acceptSolution = v->asBool();
    }

    // 备用路径：题目页没给这个字段时，去题解页读顶层 data.acceptSolution
    // （注意：题解页的 data.problem 是精简对象，不含 acceptSolution）
    if (!rep.hasAcceptSolution) {
        HttpResponse r2 = fetchPage("https://www.luogu.com.cn/problem/solution/" + pid, cookie, timeoutSec);
        JVal root2;
        if (r2.ok() && extractLentilleContext(r2.body, root2)) {
            const JVal* d2 = root2.get("data");
            const JVal* v = d2 ? d2->get("acceptSolution") : nullptr;
            if (v && v->type == JVal::BOOL) {
                rep.hasAcceptSolution = true;
                rep.acceptSolution = v->asBool();
            }
        }
    }

    if (login == LoginState::Anonymous) {
        rep.verdict = "未知（需登录）";
        rep.detail = "未提供 Cookie：匿名访问时 acceptSolution 恒为 false，无法判断";
        return rep;
    }
    if (login == LoginState::Invalid) {
        rep.verdict = "未知（Cookie 无效）";
        rep.detail = "Cookie 没通过登录校验，acceptSolution 的 false 不可作为依据";
        return rep;
    }
    if (login == LoginState::Unknown) {
        rep.verdict = "未知（登录态未确认）";
        rep.detail = "登录探针异常，无法判断 acceptSolution 的含义";
        return rep;
    }
    // —— 已确认登录 ——
    if (!rep.hasAcceptSolution) {
        rep.verdict = "未知（缺字段）";
        rep.detail = "页面和题解页都没有 acceptSolution 字段，洛谷前端可能改版了";
        return rep;
    }
    if (rep.acceptSolution) {
        rep.verdict = "可提交题解";
        rep.detail = "acceptSolution=true：题目右侧会显示「提交题解」按钮";
    } else {
        rep.verdict = "不可提交";
        rep.detail = "洛谷原话：本题目已不接受新题解提交（题解数量已足够多、题目过于简单或处于月赛保护期）";
    }
    return rep;
}

static void sleepMs(int ms) {
    if (ms <= 0) return;
    std::this_thread::sleep_for(std::chrono::milliseconds(ms));
}

// 每抓完一题随机等待 [minMs, maxMs]，模拟真人节奏，降低被风控的概率
static int randomDelay(int minMs, int maxMs) {
    if (minMs < 0) minMs = 0;
    if (maxMs < minMs) maxMs = minMs;
    static std::mt19937 rng(std::random_device{}());
    std::uniform_int_distribution<int> dist(minMs, maxMs);
    return dist(rng);
}

// ---------------------------------------------------------------------------
// 题目列表枚举（按难度 / 难度区间 / 关键字）
// ---------------------------------------------------------------------------
struct DiffSpec {
    // 传进来的原始难度值集合，保持去重后升序；空表示不限难度。
    std::vector<int> values;

    bool isAny() const { return values.empty() || (values.size() == 1 && values[0] == kAnyDifficulty); }

    // 拼成难度描述，形如「入门」「普及-~普及/提高-」「入门,普及-」
    std::string describe() const {
        if (isAny()) return "全部难度";
        // 连续三段以上就用区间写法
        if (values.size() >= 3) {
            bool contiguous = true;
            for (size_t i = 1; i < values.size(); ++i)
                if (values[i] != values[i - 1] + 1) contiguous = false;
            if (contiguous) return diffName(values.front()) + "~" + diffName(values.back());
        }
        std::string s;
        for (size_t i = 0; i < values.size(); ++i) {
            if (i) s += ",";
            s += diffName(values[i]);
        }
        return s;
    }

    // 组 URL 上的 difficulty= 参数；不限难度时返回空串
    std::string queryValue() const {
        if (isAny()) return std::string();
        std::string s;
        for (size_t i = 0; i < values.size(); ++i) {
            if (i) s += ",";
            s += std::to_string(values[i]);
        }
        return s;
    }
};

// 支持：入门 / 普及- / 普及-至普及 / 1 / 1,2 / 1-3 / all / 0 / any
static bool parseDifficulty(const std::string& raw, DiffSpec& out, std::string& err) {
    out.values.clear();
    std::vector<std::string> tokens;
    std::string cur;
    for (char c : raw) {
        if (c == ',' || c == ';' || c == ' ' || c == '|') {
            if (!cur.empty()) {
                tokens.push_back(cur);
                cur.clear();
            }
        } else if (c != '\t' && c != '\n' && c != '\r') {
            cur += c;
        }
    }
    if (!cur.empty()) tokens.push_back(cur);
    if (tokens.empty()) {
        // 空串等价于不限
        out.values.push_back(kAnyDifficulty);
        return true;
    }
    for (auto& t : tokens) {
        std::string s;
        for (char c : t)  // 去掉首尾空白
        {
            if (c == ' ' || c == '\t') continue;
            s += c;
        }
        std::string lower(s);
        for (auto& c : lower) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        if (lower == "all" || lower == "any" || lower == "*" || lower == "全部" || s == "不限" || s == "0") {
            out.values.push_back(kAnyDifficulty);
            continue;
        }
        // 数字区间「1-3」或中文区间「入门至普及-」
        auto toIdx = [](const std::string& x, int& idx) {
            if (x.size() == 1 && std::isdigit(static_cast<unsigned char>(x[0]))) {
                idx = x[0] - '0';
                return idx >= 0 && idx <= kMaxDifficulty;
            }
            for (int i = 0; i <= kMaxDifficulty; ++i)
                if (x == kDiffName[i]) {
                    idx = i;
                    return true;
                }
            return false;
        };
        int idx = 0;
        if (toIdx(s, idx)) {
            out.values.push_back(idx);
            continue;
        }
        // 区间：先用最长匹配把两端都试一遍。
        // 注意难度名本身就带 '-'（如「普及/提高-」），所以不能简单地在第一个 '-' 处切分。
        bool matched = false;
        auto tryRange = [&](size_t p, size_t seplen) {
            std::string a = s.substr(0, p), b = s.substr(p + seplen);
            if (a.empty() || b.empty()) return false;
            int ia = 0, ib = 0;
            if (!toIdx(a, ia) || !toIdx(b, ib)) return false;
            for (int i = std::min(ia, ib); i <= std::max(ia, ib); ++i) out.values.push_back(i);
            return true;
        };
        // 先试「至 / 到」（不会和难度名冲突）
        for (const char* sep : {"至", "到"}) {
            size_t p = s.find(sep);
            if (p != std::string::npos && tryRange(p, std::string(sep).size())) {
                matched = true;
                break;
            }
        }
        // 再试 '-'：从左到右每个位置都试，直到两端都能认出来
        if (!matched) {
            for (size_t p = 0; p < s.size() && !matched; ++p) {
                if (s[p] == '-') matched = tryRange(p, 1);
            }
        }
        // 最后试「~」
        if (!matched) {
            size_t p = s.find('~');
            if (p != std::string::npos) matched = tryRange(p, 1);
        }
        if (matched) continue;
        err = "无法识别的难度写法: " + t + "（可选：入门/普及-/普及/提高-/普及+/提高/提高+/省选-/省选/NOI-/NOI/NOI+/CTSC 或 1..7 或 all）";
        return false;
    }
    std::sort(out.values.begin(), out.values.end());
    out.values.erase(std::unique(out.values.begin(), out.values.end()), out.values.end());
    // 「all」会解析成 0；区间可能把 0 也扩进来（如 0-2），这时统一表示不限难度
    if (!out.values.empty() && out.values.front() == kAnyDifficulty) {
        out.values.assign(1, kAnyDifficulty);
    }
    return true;
}

// 解析页码区间「1-3」「5」；返回是否成功
static bool parsePageRange(const std::string& raw, int& first, int& last) {
    first = last = 1;
    if (raw.empty()) return true;
    size_t p = raw.find('-');
    std::string a = (p == std::string::npos) ? raw : raw.substr(0, p);
    std::string b = (p == std::string::npos) ? raw : raw.substr(p + 1);
    if (a.empty()) return false;
    for (char c : a)
        if (!std::isdigit(static_cast<unsigned char>(c))) return false;
    for (char c : b)
        if (!std::isdigit(static_cast<unsigned char>(c))) return false;
    first = std::atoi(a.c_str());
    last = (p == std::string::npos || b.empty()) ? first : std::atoi(b.c_str());
    if (first < 1) first = 1;
    if (last < first) last = first;
    return true;
}

struct ProblemListEntry {
    std::string pid;
    std::string name;
    int difficulty = -1;
};

struct ListPage {
    int count = 0;
    int perPage = 50;
    std::vector<ProblemListEntry> items;
};

// URL 百分号编码（只保留 RFC3986 unreserved 字符），让中文关键字能安全拼进 URL
static std::string urlEncode(const std::string& s) {
    static const char* hex = "0123456789ABCDEF";
    std::string out;
    for (unsigned char c : s) {
        if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-' || c == '_' ||
            c == '.' || c == '~') {
            out += static_cast<char>(c);
        } else {
            out += '%';
            out += hex[c >> 4];
            out += hex[c & 0x0F];
        }
    }
    return out;
}

// 拉取题目列表的某一页。diff.queryValue() 为空时表示不限难度；
// keyword 非空时交给服务端过滤（洛谷列表接口支持 keyword= 参数，可与 difficulty 叠加）。
static bool fetchListPage(const DiffSpec& diff, int page, const std::string& cookie, int timeoutSec, int retry,
                          ListPage& out, std::string& err, const std::string& keyword = std::string()) {
    std::string url = "https://www.luogu.com.cn/problem/list?page=" + std::to_string(page);
    std::string dv = diff.queryValue();
    if (!dv.empty()) url += "&difficulty=" + dv;
    if (!keyword.empty()) url += "&keyword=" + urlEncode(keyword);
    url += "&_contentOnly=1";

    HttpResponse r;
    bool parsed = false;
    for (int attempt = 0; attempt <= retry && !parsed; ++attempt) {
        std::string reason;
        r = fetchPage(url, cookie, timeoutSec, &reason);
        if (!r.ok()) {
            err = r.err.empty() ? "空响应" : r.err;
            if (r.status >= 400 && r.status < 500) return false;
        } else {
            JVal root;
            if (extractLentilleContext(r.body, root)) {
                const JVal* data = root.get("data");
                const JVal* problems = data ? data->get("problems") : nullptr;
                if (problems) {
                    if (const JVal* v = problems->get("count")) out.count = static_cast<int>(v->asInt());
                    if (const JVal* v = problems->get("perPage")) out.perPage = static_cast<int>(v->asInt());
                    if (out.perPage <= 0) out.perPage = 50;
                    if (const JVal* res = problems->get("result")) {
                        for (const auto& it : res->arr) {
                            ProblemListEntry e;
                            if (const JVal* v = it.get("pid")) e.pid = v->asStr();
                            if (const JVal* v = it.get("name")) e.name = v->asStr();
                            if (const JVal* v = it.get("difficulty")) e.difficulty = static_cast<int>(v->asInt());
                            if (!e.pid.empty()) out.items.push_back(e);
                        }
                    }
                    parsed = true;
                } else {
                    err = "列表 JSON 里没有 data.problems";
                }
            } else {
                err = reason.empty() ? "题目列表页没有 lentille-context" : reason;
            }
        }
        if (!parsed && attempt < retry) {
            std::cerr << "  [列表重试 " << (attempt + 1) << "/" << retry << "] 第 " << page << " 页: " << err << "\n";
            sleepMs(randomDelay(1200, 2500) * (attempt + 1));
        }
    }
    return parsed;
}

// 按难度枚举题号：翻页取完（或取到 --pages / --offset / --limit 的边界），并去重。
static bool enumerateProblems(const DiffSpec& diff, const std::string& cookie, int timeoutSec, int retry,
                              int pageFirst, int pageLast, int pageLimit, long long offset, long long limit,
                              const std::string& keyword, std::vector<ProblemListEntry>& out) {
    out.clear();
    std::vector<std::string> seen;
    auto alreadyHave = [&](const std::string& pid) {
        for (const auto& s : seen)
            if (s == pid) return true;
        return false;
    };

    // 先取起始页拿总数，算出一共有多少页
    ListPage first;
    std::string err;
    int beginPage = std::max(pageFirst, 1);
    if (!fetchListPage(diff, beginPage, cookie, timeoutSec, retry, first, err, keyword)) {
        std::cerr << "取题目列表失败（第 " << beginPage << " 页）: " << err << "\n";
        return false;
    }
    int totalPages = (first.count + first.perPage - 1) / first.perPage;
    if (totalPages < 1) totalPages = 1;

    int endPage = pageLast > 0 ? std::min(pageLast, totalPages) : totalPages;
    if (pageLimit > 0 && endPage - beginPage + 1 > pageLimit) endPage = beginPage + pageLimit - 1;
    if (endPage < beginPage) endPage = beginPage;

    std::cout << "难度: " << diff.describe() << (keyword.empty() ? "" : ("，关键字: " + keyword))
              << "\n列表共 " << first.count << " 道题，每页 " << first.perPage << " 条，共 " << totalPages
              << " 页；本次抓取第 " << beginPage << "~" << endPage << " 页";
    if (offset > 0) std::cout << "，跳过前 " << offset << " 道";
    if (limit > 0) std::cout << "，最多取 " << limit << " 道";
    std::cout << "\n\n";

    long long skipped = 0;
    auto take = [&](const std::vector<ProblemListEntry>& items) {
        for (const auto& e : items) {
            if (alreadyHave(e.pid)) continue;
            seen.push_back(e.pid);
            if (skipped < offset) {
                ++skipped;
                continue;
            }
            if (limit > 0 && static_cast<long long>(out.size()) >= limit) continue;
            out.push_back(e);
        }
    };

    // 从起始页开始按顺序往后翻，保证 --offset / --limit 落在正确的题号上
    take(first.items);
    auto progress = [&](int pg) {
        std::cout << "\r列表进度 " << pg << "/" << endPage << " 页，已跳过 " << skipped << " 道，已取 "
                  << out.size() << " 道";
        std::cout.flush();
    };
    progress(beginPage);

    for (int pg = beginPage + 1; pg <= endPage; ++pg) {
        if (limit > 0 && static_cast<long long>(out.size()) >= limit) break;  // 已取够，不必继续翻页
        sleepMs(randomDelay(600, 1500));  // 列表页之间也留点间隔
        ListPage lp;
        std::string e2;
        if (!fetchListPage(diff, pg, cookie, timeoutSec, retry, lp, e2, keyword)) {
            std::cerr << "\n取题目列表失败（第 " << pg << " 页）: " << e2 << "，已枚举的题号继续用\n";
            break;
        }
        take(lp.items);
        progress(pg);
    }
    std::cout << "\n\n";
    return true;
}

static std::vector<std::string> readIdFile(const std::string& path) {
    std::vector<std::string> ids;
    std::string content;
    if (!readWholeFile(path, content)) {
        std::cerr << "打不开题号文件: " << path << "\n";
        return ids;
    }
    std::istringstream fin(content);
    std::string line;
    while (std::getline(fin, line)) {
        std::string cur;
        for (char c : line) {
            if (c == ',' || c == ';' || c == ' ' || c == '\t' || c == '\r') {
                if (!cur.empty()) {
                    ids.push_back(cur);
                    cur.clear();
                }
            } else {
                cur += c;
            }
        }
        if (!cur.empty()) ids.push_back(cur);
    }
    // 题号文件里可能出现重复（比如从不同难度列表各复制一次），这里去重保序
    std::vector<std::string> uniq;
    for (auto& x : ids) {
        if (std::find(uniq.begin(), uniq.end(), x) == uniq.end()) uniq.push_back(x);
    }
    if (uniq.size() != ids.size())
        std::cout << "题号文件里原有 " << ids.size() << " 个题号，去重后剩 " << uniq.size() << " 个\n";
    return uniq;
}

// 按终端显示宽度补齐（UTF-8 中文按 2 列算），让表格能对齐
static std::string pad(const std::string& s, size_t width) {
    size_t cols = 0;
    for (size_t i = 0; i < s.size();) {
        unsigned char c = static_cast<unsigned char>(s[i]);
        unsigned cp = 0;
        size_t len = 1;
        if (c < 0x80) {
            cp = c;
            len = 1;
        } else if ((c >> 5) == 0x6) {
            cp = c & 0x1F;
            len = 2;
        } else if ((c >> 4) == 0xE) {
            cp = c & 0x0F;
            len = 3;
        } else if ((c >> 3) == 0x1E) {
            cp = c & 0x07;
            len = 4;
        }
        for (size_t k = 1; k < len && i + k < s.size(); ++k)
            cp = (cp << 6) | (static_cast<unsigned char>(s[i + k]) & 0x3F);
        i += len;
        cols += (cp >= 0x1100 && (cp <= 0x115F || cp == 0x2329 || cp == 0x232A ||
                                  (cp >= 0x2E80 && cp <= 0xA4CF && cp != 0x303F) ||
                                  (cp >= 0xAC00 && cp <= 0xD7A3) || (cp >= 0xF900 && cp <= 0xFAFF) ||
                                  (cp >= 0xFE30 && cp <= 0xFE6F) || (cp >= 0xFF00 && cp <= 0xFF60) ||
                                  (cp >= 0xFFE0 && cp <= 0xFFE6)))
                    ? 2
                    : 1;
    }
    std::string out = s;
    while (cols < width) {
        out += ' ';
        ++cols;
    }
    return out;
}

static void printDifficultyTable(const std::string& cookie, int timeoutSec) {
    std::cout << "序号  " << pad("难度", 18) << "题目数\n";
    std::cout << "----  " << pad("", 18) << "------\n";
    for (int d = 1; d <= kMaxDifficulty; ++d) {
        DiffSpec spec;
        spec.values.push_back(d);
        ListPage lp;
        std::string err;
        long long cnt = -1;
        if (fetchListPage(spec, 1, cookie, timeoutSec, 2, lp, err)) cnt = lp.count;
        std::cout << " " << d << "    " << pad(kDiffName[d], 18);
        if (cnt < 0)
            std::cout << "(获取失败)\n";
        else
            std::cout << cnt << "\n";
        sleepMs(randomDelay(400, 900));
    }
    std::cout << "\n说明：1..7 对应上表的序号，可直接传给 --difficulty；传 all / 0 表示不限难度。\n";
}

// ---------------------------------------------------------------------------
// 命令行参数
// ---------------------------------------------------------------------------
#ifdef _WIN32
// MinGW 的 main(int, char**) 拿到的是 GBK（ANSI）命令行，中文参数会变成乱码。
// 这里改用宽字符命令行 + UTF-8 转换，保证 --difficulty 入门 这样的中文参数可用。
static std::string w2u(const std::wstring& w) {
    if (w.empty()) return std::string();
    int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), nullptr, 0, nullptr, nullptr);
    std::string s(static_cast<size_t>(n), '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), &s[0], n, nullptr, nullptr);
    return s;
}

// 返回 UTF-8 编码的 argv
static std::vector<std::string> getArgs(int argc, char** argv) {
    std::vector<std::string> out;
    int wargc = 0;
    LPWSTR* wargv = CommandLineToArgvW(GetCommandLineW(), &wargc);
    if (wargv) {
        out.reserve(static_cast<size_t>(wargc));
        for (int i = 0; i < wargc; ++i) out.push_back(w2u(wargv[i]));
        LocalFree(wargv);
        if (!out.empty()) return out;
    }
    for (int i = 0; i < argc; ++i) out.push_back(argv[i]);
    return out;
}
#else
static std::vector<std::string> getArgs(int argc, char** argv) {
    std::vector<std::string> out;
    for (int i = 0; i < argc; ++i) out.push_back(argv[i]);
    return out;
}
#endif

static void printUsage() {
    std::cout <<
        "用法:\n"
        "  luogu_check [选项] <题号...>                 检查给定题号\n"
        "  luogu_check --difficulty <难度> [选项]       按难度自动枚举题目再检查\n"
        "\n"
        "挖掘难度的选项:\n"
        "  -d, --difficulty <难度>   难度筛选，可写：入门 / 普及- / 普及/提高- / 普及+/提高 /\n"
        "                            提高+/省选- / 省选/NOI- / NOI/NOI+/CTSC，也可写序号 1..7；\n"
        "                            支持逗号并列与区间：\"入门,普及-\"、\"入门至普及/提高-\"、1-3；\n"
        "                            all / 0 表示不限难度\n"
        "  --pages <起-止>           只用题目列表的第几页，如 1-3；默认从头翻到尾\n"
        "  --offset <n>              跳过枚举结果里的前 n 道（配合 --limit 分片跑）\n"
        "  --limit <n>               最多取 n 道题，默认不限\n"
        "  --keyword <词>            只保留题名/题号包含该词的题目\n"
        "  --list-difficulty         只打印各难度题目数量后退出\n"
        "\n"
        "通用选项:\n"
        "  --cookie <串|文件>   登录 Cookie（原始串，或含 Cookie 的文本文件 / cookies.txt）\n"
        "  -f, --file <文件>    从文件读题号（空格/逗号/分号分隔），可与 --difficulty 叠加\n"
        "  --csv <文件>         结果另存为 CSV（UTF-8 带 BOM，Excel 可直接打开）\n"
        "  --delay <毫秒>       固定间隔（等价于把 min/max 设成同一个值）\n"
        "  --delay-min <毫秒>   每抓完一题最少等待，默认 3000\n"
        "  --delay-max <毫秒>   每抓完一题最多等待，默认 10000\n"
        "  --timeout <秒>       单次请求超时，默认 20\n"
        "  --retry <次数>       失败重试次数，默认 2\n"
        "  -h, --help           显示帮助\n"
        "\n"
        "示例:\n"
        "  luogu_check --list-difficulty\n"
        "  luogu_check --difficulty 入门 --cookie cookie.txt --limit 5\n"
        "  luogu_check -d 普及-,普及/提高- --pages 1-2 --csv 结果.csv\n"
        "  luogu_check -d all --offset 100 --limit 50 --delay 4000\n";
}

int main(int argc, char** argv) {
#ifdef _WIN32
    SetConsoleOutputCP(CP_UTF8);
#endif
    std::vector<std::string> args = getArgs(argc, argv);
    argc = static_cast<int>(args.size());
    std::vector<std::string> ids;
    std::string cookie;
    std::string csvPath;
    std::string idFile;
    std::string diffArg;
    std::string pageArg;
    std::string keyword;
    bool diffGiven = false;
    bool listDiff = false;
    long long offset = 0;
    long long limit = 0;
    int delayMin = 3000;
    int delayMax = 10000;
    int timeoutSec = 20;
    int retry = 2;

    for (int i = 1; i < argc; ++i) {
        std::string a = args[static_cast<size_t>(i)];
        auto need = [&](const char* name) -> std::string {
            if (i + 1 >= argc) {
                std::cerr << "缺少参数: " << name << "\n";
                std::exit(2);
            }
            return args[static_cast<size_t>(++i)];
        };
        if (a == "-h" || a == "--help") {
            printUsage();
            return 0;
        } else if (a == "--cookie")
            cookie = loadCookie(need("--cookie"));
        else if (a == "-f" || a == "--file")
            idFile = need("--file");
        else if (a == "--csv")
            csvPath = need("--csv");
        else if (a == "-d" || a == "--difficulty") {
            diffArg = need("--difficulty");
            diffGiven = true;
        } else if (a == "--pages")
            pageArg = need("--pages");
        else if (a == "--offset")
            offset = std::atoll(need("--offset").c_str());
        else if (a == "--limit")
            limit = std::atoll(need("--limit").c_str());
        else if (a == "--keyword")
            keyword = need("--keyword");
        else if (a == "--list-difficulty")
            listDiff = true;
        else if (a == "--delay") {
            delayMin = delayMax = std::atoi(need("--delay").c_str());
        } else if (a == "--delay-min")
            delayMin = std::atoi(need("--delay-min").c_str());
        else if (a == "--delay-max")
            delayMax = std::atoi(need("--delay-max").c_str());
        else if (a == "--timeout")
            timeoutSec = std::atoi(need("--timeout").c_str());
        else if (a == "--retry")
            retry = std::atoi(need("--retry").c_str());
        else if (!a.empty() && a[0] == '-') {
            std::cerr << "未知选项: " << a << "\n";
            return 2;
        } else
            ids.push_back(a);
    }
    if (offset < 0) offset = 0;
    if (limit < 0) limit = 0;

    if (listDiff) {
        printDifficultyTable(cookie, timeoutSec);
        return 0;
    }

    int pageFirst = 1, pageLast = 0;
    if (!pageArg.empty() && !parsePageRange(pageArg, pageFirst, pageLast)) {
        std::cerr << "--pages 写法不对: " << pageArg << "（示例：1 或 1-3）\n";
        return 2;
    }

    DiffSpec diff;
    if (diffGiven) {
        std::string err;
        if (!parseDifficulty(diffArg, diff, err)) {
            std::cerr << err << "\n";
            return 2;
        }
    } else {
        diff.values.clear();  // 没给 --difficulty，就不枚举列表
    }

    if (!idFile.empty()) {
        for (auto& x : readIdFile(idFile)) ids.push_back(x);
    }

    LoginInfo login;
    if (cookie.empty()) {
        login.state = LoginState::Anonymous;
    } else {
        if (cookie.find("__client_id") == std::string::npos) {
            std::cerr << "[警告] Cookie 里没有 __client_id，几乎肯定不是登录态。\n";
        }
        std::cout << "正在校验 Cookie 登录态...";
        std::cout.flush();
        login = probeLogin(cookie, timeoutSec);
        if (login.state == LoginState::Verified) {
            std::cout << " 通过（uid=" << login.uid << "，用户名 " << login.name << "）\n\n";
        } else if (login.state == LoginState::Invalid) {
            std::cout << " 失败（Cookie 已失效或不完整，请重新复制）\n\n";
        } else {
            std::cout << " 未确认（探针请求异常，结论会标注为「未知」）\n\n";
        }
    }

    // 按难度枚举题号并接到已有题号后面
    if (diffGiven) {
        std::vector<ProblemListEntry> listed;
        if (!enumerateProblems(diff, cookie, timeoutSec, retry, pageFirst, pageLast, 0, offset, limit, keyword,
                               listed)) {
            if (ids.empty()) return 1;
        }
        for (auto& e : listed) ids.push_back(e.pid);

        // 打印枚举结果的概览（前 10 行），让用户确认抓的是不是想要的题
        size_t showN = std::min<size_t>(listed.size(), 10);
        for (size_t i = 0; i < showN; ++i)
            std::printf("  %s %s %s\n", pad(listed[i].pid, 8).c_str(), pad(diffName(listed[i].difficulty), 12).c_str(),
                        listed[i].name.c_str());
        if (listed.size() > showN) std::cout << "  ...（共 " << listed.size() << " 道）\n";
        std::cout << "\n";
    }

    if (ids.empty()) {
        std::cerr << "没有要检查的题目：请给出题号，或用 --difficulty 按难度枚举。\n\n";
        printUsage();
        return 2;
    }

    if (cookie.empty()) {
        std::cout << "[提示] 未提供 Cookie：未登录时 acceptSolution 恒为 false，"
                     "所有题目都会显示「未知（需登录）」。\n\n";
    }

    std::vector<ProblemReport> reports;
    reports.reserve(ids.size());

    std::cout << "共 " << ids.size() << " 道题目，开始抓取...\n\n";
    for (size_t i = 0; i < ids.size(); ++i) {
        const std::string& pid = ids[i];
        ProblemReport rep;
        for (int attempt = 0; attempt <= retry; ++attempt) {
            rep = checkProblem(pid, cookie, timeoutSec, login.state);
            if (rep.verdict != "抓取失败") break;
            // 4xx 是确定性错误（题号不存在等），重试没有意义
            if (rep.httpStatus >= 400 && rep.httpStatus < 500) break;
            if (attempt < retry) {
                std::cerr << "  [重试 " << (attempt + 1) << "/" << retry << "] " << pid << " : " << rep.detail
                          << "\n";
                sleepMs(randomDelay(1200, 2500) * (attempt + 1));
            }
        }
        reports.push_back(rep);

        std::printf("[%2zu/%2zu] %s %s %s %s\n", i + 1, ids.size(), pad(rep.pid, 7).c_str(),
                    pad(diffName(rep.difficulty), 10).c_str(), pad(rep.name, 34).c_str(), rep.verdict.c_str());
        if (rep.verdict == "抓取失败" || rep.verdict == "不可提交" || rep.verdict.rfind("未知", 0) == 0)
            std::printf("        └ %s\n", rep.detail.c_str());

        if (i + 1 < ids.size()) {
            int waitMs = randomDelay(delayMin, delayMax);
            std::cout << "        · 等待 " << (waitMs / 1000.0) << " 秒后继续...\n";
            std::cout.flush();
            sleepMs(waitMs);
        }
    }

    // 汇总
    int canCnt = 0, cannotCnt = 0, unknownCnt = 0, failCnt = 0;
    for (auto& r : reports) {
        if (r.verdict == "可提交题解")
            ++canCnt;
        else if (r.verdict == "不可提交")
            ++cannotCnt;
        else if (r.verdict.rfind("未知", 0) == 0)
            ++unknownCnt;
        else
            ++failCnt;
    }
    std::printf("\n===== 汇总 =====\n可提交 %d 道 | 不可提交 %d 道 | 未知 %d 道 | 失败 %d 道\n", canCnt,
                cannotCnt, unknownCnt, failCnt);

    if (!csvPath.empty()) {
        std::ostringstream fout;
        fout << "\xEF\xBB\xBF";  // UTF-8 BOM
        fout << "题号,题目,难度,类型,提交数,通过数,通过率,acceptSolution,结论,说明\n";
        for (auto& r : reports) {
            std::string name = r.name, detail = r.detail;
            for (auto& c : name)
                if (c == '"') c = '\'';
            for (auto& c : detail)
                if (c == '"') c = '\'';
            double rate = r.submit > 0 ? 100.0 * static_cast<double>(r.accepted) / static_cast<double>(r.submit)
                                       : 0.0;
            char buf[32];
            std::snprintf(buf, sizeof(buf), "%.1f", rate);
            fout << r.pid << ",\"" << name << "\"," << diffName(r.difficulty) << "," << r.type << "," << r.submit
                 << "," << r.accepted << "," << buf << "%," << (r.hasAcceptSolution ? (r.acceptSolution ? "true" : "false") : "n/a")
                 << "," << r.verdict << ",\"" << detail << "\"\n";
        }
        if (!writeWholeFile(csvPath, fout.str())) {
            std::cerr << "写不了 CSV: " << csvPath << "\n";
            return 1;
        }
        std::cout << "CSV 已写入: " << csvPath << "\n";
    }
    return failCnt > 0 ? 1 : 0;
}
