// ThumbSeek: Explorer thumbnail handler that shows the video frame at a chosen % of the duration (default 25%).
// Decodes with FFmpeg (DLLs next to this one), so it handles practically every video format.
// Install with ThumbSeek-setup.exe, or: regsvr32 ThumbSeek.dll from an admin prompt (machine-wide).
// Uninstall: regsvr32 /u ThumbSeek.dll (restores Windows' own thumbnail handler).
#include <windows.h>
#include <shlwapi.h>
#include <shlobj.h>
#include <thumbcache.h>
#include <new>
extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libswscale/swscale.h>
}

static const CLSID CLSID_ThumbSeek = {0xfe46c886, 0xe672, 0x45e5, {0x8b, 0x96, 0xdd, 0x23, 0xc2, 0x99, 0xb6, 0x4f}};
static const wchar_t kClsid[] = L"{FE46C886-E672-45E5-8B96-DD23C299B64F}";
static const wchar_t kClsidKey[] = L"Software\\Classes\\CLSID\\{FE46C886-E672-45E5-8B96-DD23C299B64F}";

// Where in the video to take the frame: the "Percent" value (0-99) the setup program stores on our CLSID key.
static double Offset() {
    DWORD pct = 25, cb = sizeof(pct);
    RegGetValueW(HKEY_LOCAL_MACHINE, kClsidKey, L"Percent", RRF_RT_REG_DWORD, nullptr, &pct, &cb);
    return (pct > 99 ? 25 : pct) / 100.0;
}
static const wchar_t kThumbProviderKey[] = L"{e357fccd-a995-4576-b01f-234630154e96}";
static const wchar_t* kExts[] = {L".mp4", L".m4v",  L".mov",  L".mkv", L".webm", L".wmv", L".asf", L".avi",
                                 L".divx", L".ts",  L".m2ts", L".mts", L".m2t",  L".3gp", L".3g2", L".flv",
                                 L".f4v", L".mpg",  L".mpeg", L".mpe", L".m2v",  L".vob", L".rm",  L".rmvb",
                                 L".ogv", L".ogm",  L".mxf",  L".dv"};

static HMODULE g_module;
static LONG g_refs;

// FFmpeg reads the video through the IStream Explorer gives us. We run inside Explorer
// (DisableProcessIsolation), so these reads are direct and fast.
static int ReadStream(void* opaque, uint8_t* buf, int size) {
    ULONG got = 0;
    if (FAILED(((IStream*)opaque)->Read(buf, size, &got))) return AVERROR(EIO);
    return got ? (int)got : AVERROR_EOF;
}

static int64_t SeekStream(void* opaque, int64_t offset, int whence) {
    IStream* s = (IStream*)opaque;
    STATSTG st;
    if (whence & AVSEEK_SIZE) return SUCCEEDED(s->Stat(&st, STATFLAG_NONAME)) ? (int64_t)st.cbSize.QuadPart : -1;
    whence &= ~AVSEEK_FORCE;
    DWORD origin = whence == SEEK_CUR ? STREAM_SEEK_CUR : whence == SEEK_END ? STREAM_SEEK_END : STREAM_SEEK_SET;
    LARGE_INTEGER li;
    li.QuadPart = offset;
    ULARGE_INTEGER pos;
    return SUCCEEDED(s->Seek(li, origin, &pos)) ? (int64_t)pos.QuadPart : -1;
}

// Everything FFmpeg allocates for one thumbnail, freed on any return path.
struct Av {
    AVIOContext* io = nullptr;
    AVFormatContext* fmt = nullptr;
    AVCodecContext* dec = nullptr;
    AVPacket* pkt = nullptr;
    AVFrame* frame = nullptr;
    SwsContext* sws = nullptr;
    uint8_t* scaled = nullptr;
    ~Av() {
        av_free(scaled);
        sws_freeContext(sws);
        av_frame_free(&frame);
        av_packet_free(&pkt);
        avcodec_free_context(&dec);
        avformat_close_input(&fmt);  // does not free our custom io
        if (io) av_freep(&io->buffer);
        avio_context_free(&io);
    }
};

static HRESULT GrabFrame(IStream* stream, UINT cx, HBITMAP* out) {
    Av av;
    const int kBuf = 1 << 16;
    uint8_t* buf = (uint8_t*)av_malloc(kBuf);
    if (!buf) return E_OUTOFMEMORY;
    if (!(av.io = avio_alloc_context(buf, kBuf, 0, stream, ReadStream, nullptr, SeekStream))) {
        av_free(buf);
        return E_OUTOFMEMORY;
    }
    if (!(av.fmt = avformat_alloc_context())) return E_OUTOFMEMORY;
    av.fmt->pb = av.io;
    av.fmt->flags |= AVFMT_FLAG_CUSTOM_IO;

    // The file name helps FFmpeg pick the container for formats that are hard to sniff (e.g. raw .m2v).
    char name[MAX_PATH * 3] = "";
    STATSTG st = {};
    if (SUCCEEDED(stream->Stat(&st, STATFLAG_DEFAULT)) && st.pwcsName) {
        WideCharToMultiByte(CP_UTF8, 0, st.pwcsName, -1, name, sizeof(name), nullptr, nullptr);
        CoTaskMemFree(st.pwcsName);
    }
    if (avformat_open_input(&av.fmt, name, nullptr, nullptr) < 0) return E_FAIL;
    if (avformat_find_stream_info(av.fmt, nullptr) < 0) return E_FAIL;

    // The main video stream (embedded cover art only if there is nothing else).
    int idx = -1;
    for (unsigned i = 0; i < av.fmt->nb_streams; ++i) {
        AVStream* s = av.fmt->streams[i];
        if (s->codecpar->codec_type != AVMEDIA_TYPE_VIDEO) continue;
        if (!(s->disposition & AV_DISPOSITION_ATTACHED_PIC)) { idx = (int)i; break; }
        if (idx < 0) idx = (int)i;
    }
    if (idx < 0) return E_FAIL;
    for (unsigned i = 0; i < av.fmt->nb_streams; ++i)
        if ((int)i != idx) av.fmt->streams[i]->discard = AVDISCARD_ALL;  // don't read audio etc.
    AVStream* vs = av.fmt->streams[idx];

    const AVCodec* codec = avcodec_find_decoder(vs->codecpar->codec_id);
    if (!codec || !(av.dec = avcodec_alloc_context3(codec))) return E_FAIL;
    if (avcodec_parameters_to_context(av.dec, vs->codecpar) < 0) return E_FAIL;
    av.dec->thread_count = 0;               // auto
    av.dec->thread_type = FF_THREAD_SLICE;  // frame threading would delay the first frame
    av.dec->skip_frame = AVDISCARD_NONKEY;  // decode keyframes only: fast, and never a half-decoded picture
    if (avcodec_open2(av.dec, codec, nullptr) < 0) return E_FAIL;

    // Seek to the chosen % of the duration: lands on the keyframe at or before that point. If the file has
    // no duration or index (some .flv, raw .m2v), jump to that % of the file size instead.
    double offset = Offset();
    if (offset > 0 && !(vs->disposition & AV_DISPOSITION_ATTACHED_PIC)) {
        int64_t start = av.fmt->start_time != AV_NOPTS_VALUE ? av.fmt->start_time : 0;
        bool sought = av.fmt->duration > 0 &&
                      av_seek_frame(av.fmt, -1, start + (int64_t)(av.fmt->duration * offset), AVSEEK_FLAG_BACKWARD) >= 0;
        int64_t size = avio_size(av.fmt->pb);
        if (!sought && size > 0 && !(av.fmt->iformat->flags & AVFMT_NO_BYTE_SEEK))
            av_seek_frame(av.fmt, -1, (int64_t)(size * offset), AVSEEK_FLAG_BYTE);
    }

    if (!(av.pkt = av_packet_alloc()) || !(av.frame = av_frame_alloc())) return E_OUTOFMEMORY;
    bool got = false;
    for (int n = 0; !got && n < 5000; ++n) {
        if (n == 1500) av.dec->skip_frame = AVDISCARD_DEFAULT;  // stream without flagged keyframes: take any frame
        if (av_read_frame(av.fmt, av.pkt) < 0) {  // end of file: drain the decoder
            avcodec_send_packet(av.dec, nullptr);
            got = avcodec_receive_frame(av.dec, av.frame) == 0;
            break;
        }
        if (av.pkt->stream_index == idx && avcodec_send_packet(av.dec, av.pkt) >= 0)
            got = avcodec_receive_frame(av.dec, av.frame) == 0;
        av_packet_unref(av.pkt);
    }
    AVFrame* f = av.frame;
    if (!got || f->width <= 0 || f->height <= 0) return E_FAIL;

    // Fit the display-aspect frame into cx x cx.
    AVRational sar = av_guess_sample_aspect_ratio(av.fmt, vs, f);
    double dw = f->width * (sar.num > 0 && sar.den > 0 ? av_q2d(sar) : 1.0), dh = f->height;
    LONG tw, th;
    if (dw >= dh) { tw = cx; th = max(1L, (LONG)(dh * cx / dw + 0.5)); }
    else          { th = cx; tw = max(1L, (LONG)(dw * cx / dh + 0.5)); }

    BITMAPINFO bi = {};
    bi.bmiHeader.biSize = sizeof(bi.bmiHeader);
    bi.bmiHeader.biWidth = tw;
    bi.bmiHeader.biHeight = -th;  // top-down
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;
    void* bits;
    HBITMAP bmp = CreateDIBSection(nullptr, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
    if (!bmp) return E_OUTOFMEMORY;

    // Scale into our own padded buffer, then copy the rows into the bitmap. swscale's SIMD code writes whole
    // 16-pixel blocks, so with an exact-width destination it can write past the end of each row; on the last
    // row that went past the end of the bitmap and crashed Explorer (seen when the thumbnail size equals the
    // video size, e.g. a 712x1280 video at 1280 px).
    const int rowBytes = tw * 4, stride = (rowBytes + 127) & ~63;  // aligned, with at least 64 spare bytes per row
    av.sws = sws_getContext(f->width, f->height, (AVPixelFormat)f->format, tw, th, AV_PIX_FMT_BGRA, SWS_BILINEAR,
                            nullptr, nullptr, nullptr);
    av.scaled = (uint8_t*)av_malloc((size_t)stride * (th + 1));
    uint8_t* dst[4] = {av.scaled};
    int dstStride[4] = {stride};
    if (!av.sws || !av.scaled || sws_scale(av.sws, f->data, f->linesize, 0, f->height, dst, dstStride) <= 0) {
        DeleteObject(bmp);
        return E_FAIL;
    }
    for (int y = 0; y < th; ++y) memcpy((uint8_t*)bits + (size_t)y * rowBytes, av.scaled + (size_t)y * stride, rowBytes);
    *out = bmp;
    return S_OK;
}

// We run inside Explorer, so a crash in FFmpeg on a malformed or unusual video would take Explorer down.
// Turn such a crash into "no thumbnail" for that one file instead. After a fault FFmpeg's state can't be
// trusted, so GrabFrame's cleanup is deliberately skipped (a small leak, rather than freeing corrupt state
// and crashing anyway). Faults on FFmpeg's own worker threads are not caught here. (__try can't share a
// function with C++ objects that have destructors, which is why this only forwards to GrabFrame.)
static HRESULT GrabFrameGuarded(IStream* stream, UINT cx, HBITMAP* out) {
    __try {
        return GrabFrame(stream, cx, out);
    } __except (GetExceptionCode() == EXCEPTION_ACCESS_VIOLATION || GetExceptionCode() == EXCEPTION_INT_DIVIDE_BY_ZERO ||
                        GetExceptionCode() == EXCEPTION_ILLEGAL_INSTRUCTION
                    ? EXCEPTION_EXECUTE_HANDLER
                    : EXCEPTION_CONTINUE_SEARCH) {
        *out = nullptr;
        return E_FAIL;
    }
}

class ThumbSeek : public IInitializeWithFile, public IInitializeWithStream, public IThumbnailProvider {
    LONG m_ref = 1;
    IStream* m_stream = nullptr;

public:
    ThumbSeek() { InterlockedIncrement(&g_refs); }
    ~ThumbSeek() {
        if (m_stream) m_stream->Release();
        InterlockedDecrement(&g_refs);
    }
    IFACEMETHODIMP QueryInterface(REFIID riid, void** ppv) {
        static const QITAB qit[] = {QITABENT(ThumbSeek, IInitializeWithFile), QITABENT(ThumbSeek, IInitializeWithStream),
                                    QITABENT(ThumbSeek, IThumbnailProvider), {0}};
        return QISearch(this, qit, riid, ppv);
    }
    IFACEMETHODIMP_(ULONG) AddRef() { return InterlockedIncrement(&m_ref); }
    IFACEMETHODIMP_(ULONG) Release() {
        LONG r = InterlockedDecrement(&m_ref);
        if (!r) delete this;
        return r;
    }
    // Explorer file views give a path, folder previews give a stream; both end up as a stream.
    IFACEMETHODIMP Initialize(LPCWSTR path, DWORD) {
        if (m_stream) return HRESULT_FROM_WIN32(ERROR_ALREADY_INITIALIZED);
        if (!path) return E_INVALIDARG;
        return SHCreateStreamOnFileEx(path, STGM_READ | STGM_SHARE_DENY_NONE, FILE_ATTRIBUTE_NORMAL, FALSE, nullptr,
                                      &m_stream);
    }
    IFACEMETHODIMP Initialize(IStream* stream, DWORD) {
        if (m_stream) return HRESULT_FROM_WIN32(ERROR_ALREADY_INITIALIZED);
        if (!stream) return E_INVALIDARG;
        m_stream = stream;
        m_stream->AddRef();
        return S_OK;
    }
    IFACEMETHODIMP GetThumbnail(UINT cx, HBITMAP* phbmp, WTS_ALPHATYPE* alpha) {
        if (!m_stream) return E_UNEXPECTED;
        *phbmp = nullptr;
        *alpha = WTSAT_RGB;
        av_log_set_level(AV_LOG_QUIET);
        return GrabFrameGuarded(m_stream, cx, phbmp);
    }
};

class Factory : public IClassFactory {
    LONG m_ref = 1;

public:
    Factory() { InterlockedIncrement(&g_refs); }
    ~Factory() { InterlockedDecrement(&g_refs); }
    IFACEMETHODIMP QueryInterface(REFIID riid, void** ppv) {
        static const QITAB qit[] = {QITABENT(Factory, IClassFactory), {0}};
        return QISearch(this, qit, riid, ppv);
    }
    IFACEMETHODIMP_(ULONG) AddRef() { return InterlockedIncrement(&m_ref); }
    IFACEMETHODIMP_(ULONG) Release() {
        LONG r = InterlockedDecrement(&m_ref);
        if (!r) delete this;
        return r;
    }
    IFACEMETHODIMP CreateInstance(IUnknown* outer, REFIID riid, void** ppv) {
        *ppv = nullptr;
        if (outer) return CLASS_E_NOAGGREGATION;
        ThumbSeek* t = new (std::nothrow) ThumbSeek;
        if (!t) return E_OUTOFMEMORY;
        HRESULT hr = t->QueryInterface(riid, ppv);
        t->Release();
        return hr;
    }
    IFACEMETHODIMP LockServer(BOOL lock) {
        lock ? InterlockedIncrement(&g_refs) : InterlockedDecrement(&g_refs);
        return S_OK;
    }
};

BOOL WINAPI DllMain(HINSTANCE inst, DWORD reason, void*) {
    if (reason == DLL_PROCESS_ATTACH) {
        g_module = inst;
        DisableThreadLibraryCalls(inst);
    }
    return TRUE;
}

STDAPI DllGetClassObject(REFCLSID clsid, REFIID riid, void** ppv) {
    *ppv = nullptr;
    if (clsid != CLSID_ThumbSeek) return CLASS_E_CLASSNOTAVAILABLE;
    Factory* f = new (std::nothrow) Factory;
    if (!f) return E_OUTOFMEMORY;
    HRESULT hr = f->QueryInterface(riid, ppv);
    f->Release();
    return hr;
}

STDAPI DllCanUnloadNow() { return g_refs == 0 ? S_OK : S_FALSE; }

// Machine-wide registration (HKLM, needs admin). Explorer ignores per-user (HKCU) thumbnail handlers.
// The handler each extension had before (normally Windows' own) is saved as a value on our CLSID key
// and put back on uninstall.
static LSTATUS SetString(const wchar_t* key, const wchar_t* name, const wchar_t* value) {
    return RegSetKeyValueW(HKEY_LOCAL_MACHINE, key, name, REG_SZ, value, (DWORD)((wcslen(value) + 1) * sizeof(wchar_t)));
}

static bool GetString(const wchar_t* key, const wchar_t* name, wchar_t* out, DWORD cb) {
    return RegGetValueW(HKEY_LOCAL_MACHINE, key, name, RRF_RT_REG_SZ, nullptr, out, &cb) == ERROR_SUCCESS;
}

STDAPI DllRegisterServer() {
    wchar_t dll[MAX_PATH], clsidKey[128], key[256], prev[64];
    if (!GetModuleFileNameW(g_module, dll, MAX_PATH)) return HRESULT_FROM_WIN32(GetLastError());
    swprintf_s(clsidKey, L"Software\\Classes\\CLSID\\%s", kClsid);
    swprintf_s(key, L"%s\\InprocServer32", clsidKey);
    LSTATUS r = SetString(key, nullptr, dll);
    if (!r) r = SetString(key, L"ThreadingModel", L"Apartment");
    // Run inside Explorer. In Windows' isolated thumbnail process every read is marshaled
    // between processes, which takes over a minute on large files.
    DWORD one = 1;
    if (!r) r = RegSetKeyValueW(HKEY_LOCAL_MACHINE, clsidKey, L"DisableProcessIsolation", REG_DWORD, &one, sizeof(one));
    for (size_t i = 0; !r && i < ARRAYSIZE(kExts); ++i) {
        swprintf_s(key, L"Software\\Classes\\%s\\ShellEx\\%s", kExts[i], kThumbProviderKey);
        if (GetString(key, nullptr, prev, sizeof(prev)) && _wcsicmp(prev, kClsid) != 0)
            r = SetString(clsidKey, kExts[i], prev);
        if (!r) r = SetString(key, nullptr, kClsid);
    }
    if (r) return HRESULT_FROM_WIN32(r);
    SHChangeNotify(SHCNE_ASSOCCHANGED, SHCNF_IDLIST, nullptr, nullptr);
    return S_OK;
}

STDAPI DllUnregisterServer() {
    wchar_t clsidKey[128], key[256], val[64], prev[64];
    swprintf_s(clsidKey, L"Software\\Classes\\CLSID\\%s", kClsid);
    for (const wchar_t* ext : kExts) {
        swprintf_s(key, L"Software\\Classes\\%s\\ShellEx\\%s", ext, kThumbProviderKey);
        if (!GetString(key, nullptr, val, sizeof(val)) || _wcsicmp(val, kClsid) != 0) continue;  // not ours
        if (GetString(clsidKey, ext, prev, sizeof(prev)))
            SetString(key, nullptr, prev);
        else
            RegDeleteKeyW(HKEY_LOCAL_MACHINE, key);
    }
    RegDeleteTreeW(HKEY_LOCAL_MACHINE, clsidKey);
    SHChangeNotify(SHCNE_ASSOCCHANGED, SHCNF_IDLIST, nullptr, nullptr);
    return S_OK;
}
