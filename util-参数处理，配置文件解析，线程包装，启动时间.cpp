// Copyright (c) 2014-2019 The vds Core developers
// Copyright (c) 2009-2010 Satoshi Nakamoto
// Copyright (c) 2009-2014 The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#if defined(HAVE_CONFIG_H)
#include "config/vds-config.h"
#endif

#include "util.h"

#include "chainparamsbase.h"
#include "random.h"
#include "serialize.h"
#include "sync.h"
#include "utilstrencodings.h"
#include "utiltime.h"

#include <stdarg.h>

#if (defined(__FreeBSD__) || defined(__OpenBSD__) || defined(__DragonFly__))
#include <pthread.h>
#include <pthread_np.h>
#endif

#ifndef WIN32
// for posix_fallocate
#ifdef __linux__

#ifdef _POSIX_C_SOURCE
#undef _POSIX_C_SOURCE
#endif

#define _POSIX_C_SOURCE 200112L

#endif // __linux__

#include <algorithm>
#include <fcntl.h>
#include <sys/resource.h>
#include <sys/stat.h>

#else

#ifdef _MSC_VER
#pragma warning(disable:4786)
#pragma warning(disable:4804)
#pragma warning(disable:4805)
#pragma warning(disable:4717)
#endif

#ifdef _WIN32_WINNT
#undef _WIN32_WINNT
#endif
#define _WIN32_WINNT 0x0501

#ifdef _WIN32_IE
#undef _WIN32_IE
#endif
#define _WIN32_IE 0x0501

#define WIN32_LEAN_AND_MEAN 1
#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <io.h> /* for _commit */
#include <shlobj.h>
#endif

#ifdef HAVE_SYS_PRCTL_H
#include <sys/prctl.h>
#endif

#include <boost/algorithm/string/case_conv.hpp> // for to_lower()/用于降低（）
#include <boost/algorithm/string/join.hpp>
#include <boost/algorithm/string/predicate.hpp> // for startswith() and endswith()//开始开关和结束开关（）
#include <boost/algorithm/string/split.hpp>
#include <boost/algorithm/string/classification.hpp>
#include <boost/filesystem.hpp>
#include <boost/filesystem/fstream.hpp>
#include <boost/foreach.hpp>
#include <boost/program_options/detail/config_file.hpp>
#include <boost/program_options/parsers.hpp>
#include <boost/thread.hpp>
#include <openssl/crypto.h>
#include <openssl/conf.h>

// Work around clang compilation problem in Boost 1.46:
//解决Boost 1.46中的clang编译问题：
// /usr/include/boost/program_options/detail/config_file.hpp:163:17: error: call to function 'to_internal' that is neither visible in the template definition nor found by argument-dependent lookup
//usr/include/boost/program_选项/detail/config_文件.hpp:163：17:错误：对函数“to_internal”的调用，该函数在模板定义中既不可见，也不通过依赖于参数的查找找到
// See also: http://stackoverflow.com/questions/10020179/compilation-fail-in-boost-librairies-program-options
//           http://clang.debian.net/status.php?version=3.0&key=CANNOT_FIND_FUNCTION
namespace boost
{

namespace program_options
{
std::string to_internal(const std::string&);
}

} // namespace boost/命名空间提升

using namespace std;

CCriticalSection cs_args;

//vds only features/仅vds功能
bool fMasterNode = false;
bool fLiteMode = false;

map<string, string> mapArgs;
map<string, vector<string> > mapMultiArgs;
bool fDebug = false;
bool fPrintToConsole = false;
bool fPrintToDebugLog = true;
bool fDaemon = false;
bool fServer = false;
string strMiscWarning;
bool fLogTimestamps = DEFAULT_LOGTIMESTAMPS;
bool fLogTimeMicros = DEFAULT_LOGTIMEMICROS;
bool fLogThreadNames = DEFAULT_LOGTHREADNAMES;
bool fLogIPs = DEFAULT_LOGIPS;
std::atomic<bool> fReopenDebugLog(false);
CTranslationInterface translationInterface;

//** Init OpenSSL library multithreading support */Init OpenSSL库多线程支持
static CCriticalSection** ppmutexOpenSSL;
void locking_callback(int mode, int i, const char* file, int line) NO_THREAD_SAFETY_ANALYSIS {
    if (mode & CRYPTO_LOCK)
    {
        ENTER_CRITICAL_SECTION(*ppmutexOpenSSL[i]);
    } else
    {
        LEAVE_CRITICAL_SECTION(*ppmutexOpenSSL[i]);
    }
}

// Init/初始化
class CInit
{
public:
    CInit()
    {
        // Init OpenSSL library multithreading support//Init开放式SSL库多线程支持
        ppmutexOpenSSL = (CCriticalSection**)OPENSSL_malloc(CRYPTO_num_locks() * sizeof(CCriticalSection*));
        for (int i = 0; i < CRYPTO_num_locks(); i++)
            ppmutexOpenSSL[i] = new CCriticalSection();
        CRYPTO_set_locking_callback(locking_callback);

        // OpenSSL can optionally load a config file which lists optional loadable modules and engines.
        //OpenSSL可以选择性地加载一个配置文件，其中列出了可选的可加载模块和引擎。
        // We don't use them so we don't require the config. However some of our libs may call functions
        //我们不使用它们，所以我们不需要配置。然而，我们的一些lib可能会调用函数
        // which attempt to load the config file, possibly resulting in an exit() or crash if it is missing
        //尝试加载配置文件，如果丢失，可能导致exit（）或崩溃
        // or corrupt. Explicitly tell OpenSSL not to try to load the file. The result for our libs will be
        //或者腐败。显式地告诉OpenSSL不要尝试加载文件。我们LIB的结果是
        // that the config appears to have been loaded and there are no modules/engines available.
        //配置似乎已加载，并且没有可用的模块/引擎。
        OPENSSL_no_config();
    }
    ~CInit()
    {
        // Shutdown OpenSSL library multithreading support
        //关闭OpenSSL库多线程支持
        CRYPTO_set_locking_callback(NULL);
        for (int i = 0; i < CRYPTO_num_locks(); i++)
            delete ppmutexOpenSSL[i];
        OPENSSL_free(ppmutexOpenSSL);
    }
}
instance_of_cinit;

/**
 * LogPrintf() has been broken a couple of times now
 * LogPrintf（）已经被破坏了几次
 * by well-meaning people adding mutexes in the most straightforward way.
 * 善意的人以最直接的方式添加互斥量。
 * It breaks because it may be called by global destructors during shutdown.
 * 它崩溃是因为它可能在关闭期间被全局析构函数调用。
 * Since the order of destruction of static/global objects is undefined,
 * 由于静态/全局对象的销毁顺序尚未定义，
 * defining a mutex as a global object doesn't work (the mutex gets
 * 将互斥体定义为全局对象不起作用（互斥体
 * destroyed, and then some later destructor calls OutputDebugStringF,
 * 销毁，然后一些稍后的析构函数调用输出Debug StringF，
 * maybe indirectly, and you get a core dump at shutdown trying to lock
 * 可能是间接的，你会在关机的时候得到一个堆芯
 * the mutex).互斥体）。
 */

static boost::once_flag debugPrintInitFlag = BOOST_ONCE_INIT;

/**
 * We use boost::call_once() to make sure mutexDebugLog and
 * 我们使用boost:：call_once（）来确保mutexDebugLog和
 * vMsgsBeforeOpenLog are initialized in a thread-safe manner.
 * vmsgbeforeOpenLog以线程安全的方式初始化。
 *
 * NOTE: fileout, mutexDebugLog and sometimes vMsgsBeforeOpenLog
 * 注意：fileout、mutexDebugLog，有时还有vMsgsBeforeOpenLog
 * are leaked on exit. This is ugly, but will be cleaned up by
 * 在出口处泄漏。这很难看，但会被清理干净
 * the OS/libc. When the shutdown sequence is fully audited and
 * 操作系统/libc。当停堆顺序被完全审计时
 * tested, explicit destruction of these objects can be implemented.
 * 经过测试，可以实现这些对象的显式销毁。
 */
static FILE* fileout = NULL;
static boost::mutex* mutexDebugLog = NULL;
static list<string>* vMsgsBeforeOpenLog;

/////////////////////////////////////////////////////////////////////// // qtum
static FILE* fileoutVM = NULL;
///////////////////////////////////////////////////////////////////////

static int FileWriteStr(const std::string& str, FILE* fp)
{
    return fwrite(str.data(), 1, str.size(), fp);
}

static void DebugPrintInit()
{
    assert(mutexDebugLog == NULL);
    mutexDebugLog = new boost::mutex();
    vMsgsBeforeOpenLog = new list<string>;
}

void OpenDebugLog()
{
    boost::call_once(&DebugPrintInit, debugPrintInitFlag);
    boost::mutex::scoped_lock scoped_lock(*mutexDebugLog);

    assert(fileout == NULL);
    assert(fileoutVM == NULL); // qtum
    assert(vMsgsBeforeOpenLog);
    boost::filesystem::path pathDebug = GetDataDir() / "debug.log";
    boost::filesystem::path pathDebugVM = GetDataDir() / "vm.log"; // qtum
    fileout = fopen(pathDebug.string().c_str(), "a");
    fileoutVM = fopen(pathDebugVM.string().c_str(), "a"); // qtum
    if (fileout) setbuf(fileout, NULL); // unbuffered

    // dump buffered messages from before we opened the log
    //在打开日志之前转储来自的缓冲消息
    while (!vMsgsBeforeOpenLog->empty()) {
        FileWriteStr(vMsgsBeforeOpenLog->front(), fileout);
        vMsgsBeforeOpenLog->pop_front();
    }

    ///////////////////////////////////////////// // qtum
    if (fileoutVM) {
        setbuf(fileoutVM, NULL); // unbuffered/无缓冲
        // dump buffered messages from before we opened the log/在打开日志之前转储来自的缓冲消息
        while (!vMsgsBeforeOpenLog->empty()) {
            FileWriteStr(vMsgsBeforeOpenLog->front(), fileoutVM);
            vMsgsBeforeOpenLog->pop_front();
        }
    }
    /////////////////////////////////////////////

    delete vMsgsBeforeOpenLog;
    vMsgsBeforeOpenLog = NULL;
}

bool LogAcceptCategory(const char* category)
{
    if (category != NULL) {
        if (!fDebug)
            return false;

        // Give each thread quick access to -debug settings.
        //给每个线程快速访问-debug设置。
        // This helps prevent issues debugging global destructors,
        //这有助于防止调试全局析构函数时出现问题，
        // where mapMultiArgs might be deleted before another
        //其中映射多参数可能会在另一个之前被删除
        // global destructor calls LogPrint()
        //全局析构函数调用LogPrint（）
        static boost::thread_specific_ptr<set<string> > ptrCategory;
        if (ptrCategory.get() == NULL) {
            const vector<string>& categories = mapMultiArgs["-debug"];
            ptrCategory.reset(new set<string>(categories.begin(), categories.end()));
            // thread_specific_ptr automatically deletes the set when the thread ends.
            //线程特定的\u ptr在线程结束时自动删除集合。
        }
        const set<string>& setCategories = *ptrCategory.get();

        // if not debugging everything and not debugging specific category, LogPrint does nothing.
        //如果不调试所有内容，也不调试特定的类别，LogPrint什么也不做。
        if (setCategories.count(string("")) == 0 &&
                setCategories.count(string("1")) == 0 &&
                setCategories.count(string(category)) == 0)
            return false;
    }
    return true;
}

/**
 * fStartedNewLine is a state variable held by the calling context that will
 * fStartedNewLine是由调用上下文保存的状态变量，它将
 * suppress printing of the timestamp when multiple calls are made that don't
 * 当进行多个调用时，禁止打印时间戳
 * end in a newline. Initialize it to true, and hold it, in the calling context.
 * 以换行结束。在调用上下文中将其初始化为true并保持不变。
 */
static std::string LogTimestampStr(const std::string& str, std::atomic_bool* fStartedNewLine)
{
    string strStamped;

    if (!fLogTimestamps)
        return str;

    if (*fStartedNewLine) {
        int64_t nTimeMicros = GetLogTimeMicros();
        strStamped = DateTimeStrFormat("%Y-%m-%d %H:%M:%S", nTimeMicros / 1000000);
        if (fLogTimeMicros)
            strStamped += strprintf(".%06d", nTimeMicros % 1000000);
        strStamped += ' ' + str;
    } else
        strStamped = str;

    if (!str.empty() && str[str.size() - 1] == '\n')
        *fStartedNewLine = true;
    else
        *fStartedNewLine = false;

    return strStamped;
}

int LogPrintStr(const std::string& str, bool useVMLog)
{

//////////////////////////////// // qtum
    FILE* file = fileout;
    if (useVMLog) {
        file = fileoutVM;
    }
////////////////////////////////

    int ret = 0; // Returns total number of characters written//返回写入的字符总数
    static std::atomic_bool fStartedNewLine(true);

    string strTimestamped = LogTimestampStr(str, &fStartedNewLine);

    if (fPrintToConsole) {
        // print to console/打印到控制台
        ret = fwrite(strTimestamped.data(), 1, strTimestamped.size(), stdout);
        fflush(stdout);
    } else if (fPrintToDebugLog) {
        boost::call_once(&DebugPrintInit, debugPrintInitFlag);
        boost::mutex::scoped_lock scoped_lock(*mutexDebugLog);

        // buffer if we haven't opened the log yet/缓冲区，如果我们还没有打开日志
        if (file == NULL) {
            assert(vMsgsBeforeOpenLog);
            ret = strTimestamped.length();
            vMsgsBeforeOpenLog->push_back(strTimestamped);
        } else {
            // reopen the log file, if requested/如果需要，请重新打开日志文件
            if (fReopenDebugLog) {
                fReopenDebugLog = false;
                boost::filesystem::path pathDebug = GetDataDir() / "debug.log";
                if (freopen(pathDebug.string().c_str(), "a", file) != NULL)
                    setbuf(file, NULL); // unbuffered/无缓冲
            }

            ret = FileWriteStr(strTimestamped, file);
        }
    }
    return ret;
}

static void InterpretNegativeSetting(string name, map<string, string>& mapSettingsRet)
{
    // interpret -nofoo as -foo=0 (and -nofoo=0 as -foo=1) as long as -foo not set
    //只要-foo未设置，就将-nofoo解释为-foo=0（而-nofoo=0则解释为-foo=1）
    if (name.find("-no") == 0) {
        std::string positive("-");
        positive.append(name.begin() + 3, name.end());
        if (mapSettingsRet.count(positive) == 0) {
            bool value = !GetBoolArg(name, false);
            mapSettingsRet[positive] = (value ? "1" : "0");
        }
    }
}

void ParseParameters(int argc, const char* const argv[])
{
    mapArgs.clear();
    mapMultiArgs.clear();

    for (int i = 1; i < argc; i++) {
        std::string str(argv[i]);
        std::string strValue;
        size_t is_index = str.find('=');
        if (is_index != std::string::npos) {
            strValue = str.substr(is_index + 1);
            str = str.substr(0, is_index);
        }
#ifdef WIN32
        boost::to_lower(str);
        if (boost::algorithm::starts_with(str, "/"))
            str = "-" + str.substr(1);
#endif

        if (str[0] != '-')
            break;

        // Interpret --foo as -foo./将--foo解释为-foo。
        // If both --foo and -foo are set, the last takes effect./如果同时设置了--foo和-foo，则最后一个将生效。
        if (str.length() > 1 && str[1] == '-')
            str = str.substr(1);

        mapArgs[str] = strValue;
        mapMultiArgs[str].push_back(strValue);
    }

    // New 0.6 features:新的0.6功能：
    BOOST_FOREACH(const PAIRTYPE(string, string)& entry, mapArgs) {
        // interpret -nofoo as -foo=0 (and -nofoo=0 as -foo=1) as long as -foo not set
        //只要-foo未设置，就将-nofoo解释为-foo=0（而-nofoo=0则解释为-foo=1）
        InterpretNegativeSetting(entry.first, mapArgs);
    }
}

bool IsArgSet(const std::string& strArg)
{
    LOCK(cs_args);
    return mapArgs.count(strArg);
}

std::string GetArg(const std::string& strArg, const std::string& strDefault)
{
    LOCK(cs_args);
    if (mapArgs.count(strArg))
        return mapArgs[strArg];
    return strDefault;
}

int64_t GetArg(const std::string& strArg, int64_t nDefault)
{
    LOCK(cs_args);
    if (mapArgs.count(strArg))
        return atoi64(mapArgs[strArg]);
    return nDefault;
}

bool GetBoolArg(const std::string& strArg, bool fDefault)
{
    LOCK(cs_args);
    if (mapArgs.count(strArg)) {
        if (mapArgs[strArg].empty())
            return true;
        return (atoi(mapArgs[strArg]) != 0);
    }
    return fDefault;
}

bool SoftSetArg(const std::string& strArg, const std::string& strValue)
{
    LOCK(cs_args);
    if (mapArgs.count(strArg))
        return false;
    mapArgs[strArg] = strValue;
    return true;
}

bool SoftSetBoolArg(const std::string& strArg, bool fValue)
{
    LOCK(cs_args);
    if (fValue)
        return SoftSetArg(strArg, std::string("1"));
    else
        return SoftSetArg(strArg, std::string("0"));
}

static const int screenWidth = 79;
static const int optIndent = 2;
static const int msgIndent = 7;

std::string HelpMessageGroup(const std::string& message)
{
    return std::string(message) + std::string("\n\n");
}

std::string HelpMessageOpt(const std::string& option, const std::string& message)
{
    return std::string(optIndent, ' ') + std::string(option) +
           std::string("\n") + std::string(msgIndent, ' ') +
           FormatParagraph(message, screenWidth - msgIndent, msgIndent) +
           std::string("\n\n");
}

static std::string FormatException(const std::exception* pex, const char* pszThread)
{
#ifdef WIN32
    char pszModule[MAX_PATH] = "";
    GetModuleFileNameA(NULL, pszModule, sizeof(pszModule));
#else
    const char* pszModule = "Vds";
#endif
    if (pex)
        return strprintf(
                   "EXCEPTION: %s       \n%s       \n%s in %s       \n", typeid(*pex).name(), pex->what(), pszModule, pszThread);
    else
        return strprintf(
                   "UNKNOWN EXCEPTION       \n%s in %s       \n", pszModule, pszThread);
}

void PrintExceptionContinue(const std::exception* pex, const char* pszThread)
{
    std::string message = FormatException(pex, pszThread);
    LogPrintf("\n\n************************\n%s\n", message);
    fprintf(stderr, "\n\n************************\n%s\n", message.c_str());
    strMiscWarning = message;
}

boost::filesystem::path GetDefaultDataDir()
{
    namespace fs = boost::filesystem;
    // Windows < Vista: C:\Documents and Settings\Username\Application Data\Vds
    // Windows >= Vista: C:\Users\Username\AppData\Roaming\Vds
    // Mac: ~/Library/Application Support/Vds
    // Unix: ~/.zcash
#ifdef WIN32
    // Windows
    return GetSpecialFolderPath(CSIDL_APPDATA) / "Vds";
#else
    fs::path pathRet;
    char* pszHome = getenv("HOME");
    if (pszHome == NULL || strlen(pszHome) == 0)
        pathRet = fs::path("/");
    else
        pathRet = fs::path(pszHome);
#ifdef MAC_OSX
    // Mac
    pathRet /= "Library/Application Support";
    TryCreateDirectory(pathRet);
    return pathRet / "Vds";
#else
    // Unix
    return pathRet / ".vds";
#endif
#endif
}

static boost::filesystem::path pathCached;
static boost::filesystem::path pathCachedNetSpecific;
static boost::filesystem::path zc_paramsPathCached;
static CCriticalSection csPathCached;

static boost::filesystem::path VC_GetBaseParamsDir()
{
    // Copied from GetDefaultDataDir and adapter for zcash params.
    //从GetDefaultDataDir和适配器复制zcash params。

    namespace fs = boost::filesystem;
    // Windows < Vista: C:\Documents and Settings\Username\Application Data\VdsParams
    // Windows >= Vista: C:\Users\Username\AppData\Roaming\VdsParams
    // Mac: ~/Library/Application Support/VdsParams
    // Unix: ~/.zcash-params
#ifdef WIN32
    // Windows
    return GetSpecialFolderPath(CSIDL_APPDATA) / "VdsParams";
#else
    fs::path pathRet;
    char* pszHome = getenv("HOME");
    if (pszHome == NULL || strlen(pszHome) == 0)
        pathRet = fs::path("/");
    else
        pathRet = fs::path(pszHome);
#ifdef MAC_OSX
    // Mac
    pathRet /= "Library/Application Support";
    TryCreateDirectory(pathRet);
    return pathRet / "VdsParams";
#else
    // Unix
    return pathRet / ".vds-params";
#endif
#endif
}

const boost::filesystem::path& VC_GetParamsDir()
{
    namespace fs = boost::filesystem;

    LOCK(csPathCached); // Reuse the same lock as upstream./重复使用与上游相同的锁。

    fs::path& path = zc_paramsPathCached;

    // This can be called during exceptions by LogPrintf(), so we cache the
    //这可以在异常期间由LogPrintf（）调用，因此我们缓存
    // value so we don't have to do memory allocations after that.
    //值，这样我们就不必再进行内存分配了。
    if (!path.empty())
        return path;

    path = VC_GetBaseParamsDir();

    return path;
}

// Return the user specified export directory.  Create directory if it doesn't exist.
//返回用户指定的导出目录。创建不存在的目录。
// If user did not set option, return an empty path.
//若用户未设置选项，则返回空路径。
// If there is a filesystem problem, throw an exception.
//如果文件系统有问题，抛出异常。

const boost::filesystem::path GetExportDir()
{
    namespace fs = boost::filesystem;
    fs::path path;
    if (mapArgs.count("-exportdir")) {
        path = fs::system_complete(mapArgs["-exportdir"]);
        if (fs::exists(path) && !fs::is_directory(path)) {
            throw std::runtime_error(strprintf("The -exportdir '%s' already exists and is not a directory", path.string()));
        }
        if (!fs::exists(path) && !fs::create_directories(path)) {
            throw std::runtime_error(strprintf("Failed to create directory at -exportdir '%s'", path.string()));
        }
    }
    return path;
}


const boost::filesystem::path& GetDataDir(bool fNetSpecific)
{
    namespace fs = boost::filesystem;

    LOCK(csPathCached);

    fs::path& path = fNetSpecific ? pathCachedNetSpecific : pathCached;

    // This can be called during exceptions by LogPrintf(), so we cache the
    //这可以在异常期间由LogPrintf（）调用，因此我们缓存
    // value so we don't have to do memory allocations after that.
    //值，这样我们就不必再进行内存分配了。
    if (!path.empty())
        return path;

    if (mapArgs.count("-datadir")) {
        path = fs::system_complete(mapArgs["-datadir"]);
        if (!fs::is_directory(path)) {
            path = "";
            return path;
        }
    } else {
        path = GetDefaultDataDir();
    }
    if (fNetSpecific)
        path /= BaseParams().DataDir();

    fs::create_directories(path);

    return path;
}

void ClearDatadirCache()
{
    pathCached = boost::filesystem::path();
    pathCachedNetSpecific = boost::filesystem::path();
}

boost::filesystem::path GetConfigFile()
{
    boost::filesystem::path pathConfigFile(GetArg("-conf", "vds.conf"));
    if (!pathConfigFile.is_complete())
        pathConfigFile = GetDataDir(false) / pathConfigFile;

    return pathConfigFile;
}

boost::filesystem::path GetMasternodeConfigFile()
{
    boost::filesystem::path pathConfigFile(GetArg("-mnconf", "masternode.conf"));
    if (!pathConfigFile.is_complete()) pathConfigFile = GetDataDir() / pathConfigFile;
    return pathConfigFile;
}

void ReadConfigFile(map<string, string>& mapSettingsRet,
                    map<string, vector<string> >& mapMultiSettingsRet)
{
    boost::filesystem::ifstream streamConfig(GetConfigFile());
//    if (!streamConfig.good())
//        throw missing_zcash_conf();
    if (!streamConfig.good()) {
        // Create empty dash.conf if it does not excist/创建空的仪表板.conf如果不刺激
        FILE* configFile = fopen(GetConfigFile().string().c_str(), "a");
        if (configFile != NULL)
            fclose(configFile);
        return; // Nothing to read, so just return/没什么可看的，就回去吧
    }

    set<string> setOptions;
    setOptions.insert("*");

    for (boost::program_options::detail::config_file_iterator it(streamConfig, setOptions), end; it != end; ++it) {
        // Don't overwrite existing settings so command line settings override zcash.conf
        //不要覆盖现有设置，因此命令行设置会覆盖zcash.conf公司
        string strKey = string("-") + it->string_key;
        if (mapSettingsRet.count(strKey) == 0) {
            mapSettingsRet[strKey] = it->value[0];
            // interpret nofoo=1 as foo=0 (and nofoo=0 as foo=1) as long as foo not set)
            //只要foo未设置，就将nofoo=1解释为foo=0（而nofoo=0则解释为foo=1）
            InterpretNegativeSetting(strKey, mapSettingsRet);
        }
        mapMultiSettingsRet[strKey].push_back(it->value[0]);
    }
    // If datadir is changed in .conf file:
    //如果在.conf文件中更改了datadir：
    ClearDatadirCache();
}

#ifndef WIN32
boost::filesystem::path GetPidFile()
{
    boost::filesystem::path pathPidFile(GetArg("-pid", "vdsd.pid"));
    if (!pathPidFile.is_complete()) pathPidFile = GetDataDir() / pathPidFile;
    return pathPidFile;
}

void CreatePidFile(const boost::filesystem::path& path, pid_t pid)
{
    FILE* file = fopen(path.string().c_str(), "w");
    if (file) {
        fprintf(file, "%d\n", pid);
        fclose(file);
    }
}
#endif

bool RenameOver(boost::filesystem::path src, boost::filesystem::path dest)
{
#ifdef WIN32
    return MoveFileExA(src.string().c_str(), dest.string().c_str(),
                       MOVEFILE_REPLACE_EXISTING) != 0;
#else
    int rc = std::rename(src.string().c_str(), dest.string().c_str());
    return (rc == 0);
#endif /* WIN32 */
}

/**
 * Ignores exceptions thrown by Boost's create_directory if the requested directory exists.
 * 如果请求的目录存在，则忽略Boost的create_目录引发的异常。
 * Specifically handles case where path p exists, but it wasn't possible for the user to
 * 专门处理路径p存在的情况，但用户不可能
 * write to the parent directory.
 * 写入父目录。
 */
bool TryCreateDirectory(const boost::filesystem::path& p)
{
    try {
        return boost::filesystem::create_directory(p);
    } catch (const boost::filesystem::filesystem_error&) {
        if (!boost::filesystem::exists(p) || !boost::filesystem::is_directory(p))
            throw;
    }

    // create_directory didn't create the directory, it had to have existed already
    //create_directory没有创建目录，它必须已经存在
    return false;
}

void FileCommit(FILE* fileout)
{
    fflush(fileout); // harmless if redundantly called/如果重复调用则无害
#ifdef WIN32
    HANDLE hFile = (HANDLE)_get_osfhandle(_fileno(fileout));
    FlushFileBuffers(hFile);
#else
#if defined(__linux__) || defined(__NetBSD__)
    fdatasync(fileno(fileout));
#elif defined(__APPLE__) && defined(F_FULLFSYNC)
    fcntl(fileno(fileout), F_FULLFSYNC, 0);
#else
    fsync(fileno(fileout));
#endif
#endif
}

bool TruncateFile(FILE* file, unsigned int length)
{
#if defined(WIN32)
    return _chsize(_fileno(file), length) == 0;
#else
    return ftruncate(fileno(file), length) == 0;
#endif
}

/**
 * this function tries to raise the file descriptor limit to the requested number.
 * 此函数尝试将文件描述符限制提高到请求的数目。
 * It returns the actual file descriptor limit (which may be more or less than nMinFD)
 * 它返回实际的文件描述符限制（可能大于或小于nMinFD）
 */
int RaiseFileDescriptorLimit(int nMinFD)
{
#if defined(WIN32)
    return 2048;
#else
    struct rlimit limitFD;
    if (getrlimit(RLIMIT_NOFILE, &limitFD) != -1) {
        if (limitFD.rlim_cur < (rlim_t)nMinFD) {
            limitFD.rlim_cur = nMinFD;
            if (limitFD.rlim_cur > limitFD.rlim_max)
                limitFD.rlim_cur = limitFD.rlim_max;
            setrlimit(RLIMIT_NOFILE, &limitFD);
            getrlimit(RLIMIT_NOFILE, &limitFD);
        }
        return limitFD.rlim_cur;
    }
    return nMinFD; // getrlimit failed, assume it's fine/getrlimit失败了，假设没问题
#endif
}

/**
 * this function tries to make a particular range of a file allocated (corresponding to disk space)
 * 此函数尝试分配文件的特定范围（对应于磁盘空间）
 * it is advisory, and the range specified in the arguments will never contain live data
 * 它是建议性的，参数中指定的范围永远不会包含实时数据
 */
void AllocateFileRange(FILE* file, unsigned int offset, unsigned int length)
{
#if defined(WIN32)
    // Windows-specific version
    HANDLE hFile = (HANDLE)_get_osfhandle(_fileno(file));
    LARGE_INTEGER nFileSize;
    int64_t nEndPos = (int64_t)offset + length;
    nFileSize.u.LowPart = nEndPos & 0xFFFFFFFF;
    nFileSize.u.HighPart = nEndPos >> 32;
    SetFilePointerEx(hFile, nFileSize, 0, FILE_BEGIN);
    SetEndOfFile(hFile);
#elif defined(MAC_OSX)
    // OSX specific version
    fstore_t fst;
    fst.fst_flags = F_ALLOCATECONTIG;
    fst.fst_posmode = F_PEOFPOSMODE;
    fst.fst_offset = 0;
    fst.fst_length = (off_t)offset + length;
    fst.fst_bytesalloc = 0;
    if (fcntl(fileno(file), F_PREALLOCATE, &fst) == -1) {
        fst.fst_flags = F_ALLOCATEALL;
        fcntl(fileno(file), F_PREALLOCATE, &fst);
    }
    ftruncate(fileno(file), fst.fst_length);
#elif defined(__linux__)
    // Version using posix_fallocate
    off_t nEndPos = (off_t)offset + length;
    posix_fallocate(fileno(file), 0, nEndPos);
#else
    // Fallback version/回退版本
    // TODO: just write one byte per block/TODO:每个块只写一个字节
    static const char buf[65536] = {};
    fseek(file, offset, SEEK_SET);
    while (length > 0) {
        unsigned int now = 65536;
        if (length < now)
            now = length;
        fwrite(buf, 1, now, file); // allowed to fail; this function is advisory anyway//允许失败；此功能无论如何都是建议性的
        length -= now;
    }
#endif
}

void ShrinkDebugFile()
{
    // Scroll debug.log if it's getting too big/纸卷调试日志如果它变得太大了
    boost::filesystem::path pathLog = GetDataDir() / "debug.log";
    FILE* file = fopen(pathLog.string().c_str(), "r");
    if (file && boost::filesystem::file_size(pathLog) > 10 * 1000000) {
        // Restart the file with some of the end/重新启动文件的一些结尾
        std::vector <char> vch(200000, 0);
        fseek(file, -((long)vch.size()), SEEK_END);
        int nBytes = fread(vch.data(), 1, vch.size(), file);
        fclose(file);

        file = fopen(pathLog.string().c_str(), "w");
        if (file) {
            fwrite(vch.data(), 1, nBytes, file);
            fclose(file);
        }
    } else if (file != NULL)
        fclose(file);
}

#ifdef WIN32
boost::filesystem::path GetSpecialFolderPath(int nFolder, bool fCreate)
{
    namespace fs = boost::filesystem;

    char pszPath[MAX_PATH] = "";

    if (SHGetSpecialFolderPathA(NULL, pszPath, nFolder, fCreate)) {
        return fs::path(pszPath);
    }

    LogPrintf("SHGetSpecialFolderPathA() failed, could not obtain requested path.\n");
    return fs::path("");
}
#endif

boost::filesystem::path GetTempPath()
{
#if BOOST_FILESYSTEM_VERSION == 3
    return boost::filesystem::temp_directory_path();
#else
    // TODO: remove when we don't support filesystem v2 anymore/TODO:当不再支持文件系统v2时删除
    boost::filesystem::path path;
#ifdef WIN32
    char pszPath[MAX_PATH] = "";

    if (GetTempPathA(MAX_PATH, pszPath))
        path = boost::filesystem::path(pszPath);
#else
    path = boost::filesystem::path("/tmp");
#endif
    if (path.empty() || !boost::filesystem::is_directory(path)) {
        LogPrintf("GetTempPath(): failed to find temp path\n");
        return boost::filesystem::path("");
    }
    return path;
#endif
}

void runCommand(const std::string& strCommand)
{
    int nErr = ::system(strCommand.c_str());
    if (nErr)
        LogPrintf("runCommand error: system(%s) returned %d\n", strCommand, nErr);
}

void RenameThread(const char* name)
{
#if defined(PR_SET_NAME)
    // Only the first 15 characters are used (16 - NUL terminator)
    ::prctl(PR_SET_NAME, name, 0, 0, 0);
#elif (defined(__FreeBSD__) || defined(__OpenBSD__) || defined(__DragonFly__))
    pthread_set_name_np(pthread_self(), name);

#elif defined(MAC_OSX)
    pthread_setname_np(name);
#else
    // Prevent warnings for unused parameters...防止对未使用的参数发出警告。。。
    (void)name;
#endif
}

void SetupEnvironment()
{
    // On most POSIX systems (e.g. Linux, but not BSD) the environment's locale
    //在大多数POSIX系统（例如Linux，但不是BSD）上，环境的语言环境
    // may be invalid, in which case the "C" locale is used as fallback.
    //可能无效，在这种情况下，“C”区域设置用作备用。
#if !defined(WIN32) && !defined(MAC_OSX) && !defined(__FreeBSD__) && !defined(__OpenBSD__)
    try {
        std::locale(""); // Raises a runtime error if current locale is invalid//如果当前区域设置无效，则引发运行时错误
    } catch (const std::runtime_error&) {
        setenv("LC_ALL", "C", 1);
    }
#endif
    // The path locale is lazy initialized and to avoid deinitialization errors
    //路径区域设置是延迟初始化的，以避免取消初始化错误
    // in multithreading environments, it is set explicitly by the main thread.
    //在多线程环境中，它是由主线程显式设置的。
    // A dummy locale is used to extract the internal default locale, used by
    //虚拟区域设置用于提取内部默认区域设置，由使用
    // boost::filesystem::path, which is then used to explicitly imbue the path.
    //文件系统：：path，然后用于显式地插入路径。
    std::locale loc = boost::filesystem::path::imbue(std::locale::classic());
    boost::filesystem::path::imbue(loc);
}

bool SetupNetworking()
{
#ifdef WIN32
    // Initialize Windows Sockets
    WSADATA wsadata;
    int ret = WSAStartup(MAKEWORD(2, 2), &wsadata);
    if (ret != NO_ERROR || LOBYTE(wsadata.wVersion ) != 2 || HIBYTE(wsadata.wVersion) != 2)
        return false;
#endif
    return true;
}

void SetThreadPriority(int nPriority)
{
#ifdef WIN32
    SetThreadPriority(GetCurrentThread(), nPriority);
#else // WIN32
#ifdef PRIO_THREAD
    setpriority(PRIO_THREAD, 0, nPriority);
#else // PRIO_THREAD
    setpriority(PRIO_PROCESS, 0, nPriority);
#endif // PRIO_THREAD
#endif // WIN32
}

int GetNumCores()
{
#if BOOST_VERSION >= 105600
    return boost::thread::physical_concurrency();
#else // Must fall back to hardware_concurrency, which unfortunately counts virtual cores/必须回到硬件并发，不幸的是，这会计算虚拟核心
    return boost::thread::hardware_concurrency();
#endif
}


std::string PrivacyInfo()
{
    return "\n" +
           FormatParagraph(strprintf(_("In order to ensure you are adequately protecting your privacy when using Vds, please see <%s>."),
                                     "not yet published")) + "\n";
}

uint32_t StringVersionToInt(const std::string& strVersion)
{
    std::vector<std::string> tokens;
    boost::split(tokens, strVersion, boost::is_any_of("."));
    if (tokens.size() != 3)
        throw std::bad_cast();
    uint32_t nVersion = 0;
    for (unsigned idx = 0; idx < 3; idx++) {
        if (tokens[idx].length() == 0)
            throw std::bad_cast();
        uint32_t value = boost::lexical_cast<uint32_t>(tokens[idx]);
        if (value > 255)
            throw std::bad_cast();
        nVersion <<= 8;
        nVersion |= value;
    }
    return nVersion;
}

std::string IntVersionToString(uint32_t nVersion)
{
    if ((nVersion >> 24) > 0) // MSB is always 0
        throw std::bad_cast();
    if (nVersion == 0)
        throw std::bad_cast();
    std::array<std::string, 3> tokens;
    for (unsigned idx = 0; idx < 3; idx++) {
        unsigned shift = (2 - idx) * 8;
        uint32_t byteValue = (nVersion >> shift) & 0xff;
        tokens[idx] = boost::lexical_cast<std::string>(byteValue);
    }
    return boost::join(tokens, ".");
}

std::string SafeIntVersionToString(uint32_t nVersion)
{
    try {
        return IntVersionToString(nVersion);
    } catch (const std::bad_cast&) {
        return "invalid_version";
    }
}

bool CheckHex(const std::string& str)
{
    size_t data = 0;
    if (str.size() > 2 && (str.compare(0, 2, "0x") == 0 || str.compare(0, 2, "0X") == 0)) {
        data = 2;
    }
    return str.size() > data && str.find_first_not_of("0123456789abcdefABCDEF", data) == std::string::npos;
}

uint32_t StringToInt(const std::string str)
{
    return atoi(str.c_str());
}
std::string IntToString(const int n)
{
    std::stringstream newstr;
    newstr << n;
    return newstr.str();
}
std::string LongToString(const uint64_t n)
{
    std::stringstream newstr;
    newstr << n;
    return newstr.str();
}
bool StringToBool(const std::string str)
{
    bool b;
    std::istringstream(str) >> std::boolalpha >> b;
    return b;
}
std::string BoolToString(bool ok)
{
    std::stringstream newstr;
    newstr << ok;
    return newstr.str();
}
