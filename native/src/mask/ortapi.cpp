#include "mask/ortapi.h"

#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>
#include <algorithm>
#include <atomic>
#include <mutex>
#include <vector>

#include <windows.h>
#include <winver.h>

namespace kv::mask {

namespace {

QString ortDllPath() {
  return QDir(QCoreApplication::applicationDirPath()).filePath(QStringLiteral("onnxruntime.dll"));
}

const QString& missingDllMessage() {
  static const QString m = QStringLiteral("AI の部品（onnxruntime.dll）が見つかりません。アプリを入れ直してください");
  return m;
}

/** 読み込みを一度試したか（試したあとは、その結果を使う） */
std::atomic<bool> g_attempted{false};

/**
 * onnxruntime.dll（1.31.1）は Visual C++ 14.44 で作られている。それより古いランタイムの上では、読み込めても途中で落ちることがある
 * （Microsoft のランタイムは、作ったときの版以上が要る）
 */
constexpr unsigned kMinVcMinor = 44;
const QString& oldRuntimeMessage() {
  static const QString m = QStringLiteral("AI の部品を読み込めません（Microsoft Visual C++ ランタイムが入っていないか、古いようです。"
                                          "https://aka.ms/vc14/vc_redist.x64.exe から入れてください）");
  return m;
}

/** 読み込んである DLL（name）のファイルの版。分からなければ false */
bool moduleVersion(const wchar_t* name, unsigned* major, unsigned* minor) {
  HMODULE m = GetModuleHandleW(name);
  if (!m) return false;
  std::vector<wchar_t> path(32768);
  const DWORD n = GetModuleFileNameW(m, path.data(), DWORD(path.size()));
  if (n == 0 || n >= path.size()) return false;
  DWORD ignored = 0;
  const DWORD size = GetFileVersionInfoSizeW(path.data(), &ignored);
  if (size == 0) return false;
  std::vector<char> data(size);
  if (!GetFileVersionInfoW(path.data(), 0, size, data.data())) return false;
  VS_FIXEDFILEINFO* info = nullptr;
  UINT len = 0;
  if (!VerQueryValueW(data.data(), L"\\", reinterpret_cast<void**>(&info), &len) || !info || len < sizeof(VS_FIXEDFILEINFO)) return false;
  *major = HIWORD(info->dwFileVersionMS);
  *minor = LOWORD(info->dwFileVersionMS);
  return true;
}

struct OrtState {
  const OrtApi* api = nullptr;
  OrtEnv* env = nullptr;
  OrtMemoryInfo* cpu = nullptr;
  QString error;
};

OrtState& state() {
  static OrtState s;
  static std::once_flag once;
  std::call_once(once, [] {
    // アプリと同じフォルダーの onnxruntime.dll だけを読む（Windows に入っている別の版の onnxruntime.dll を使わないように）。
    // 必要な DLL（onnxruntime_providers_shared.dll・Visual C++ ランタイム）は、同じフォルダーと Windows のフォルダーから探す
    const QString dll = ortDllPath();
    if (!QFileInfo::exists(dll)) {
      s.error = missingDllMessage();
      return;
    }
    const std::wstring path = QDir::toNativeSeparators(dll).toStdWString();
    HMODULE h = LoadLibraryExW(path.c_str(), nullptr, LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
    if (!h) {
      const DWORD e = GetLastError();
      s.error = e == ERROR_MOD_NOT_FOUND || e == ERROR_PROC_NOT_FOUND
                    ? oldRuntimeMessage()
                    : QStringLiteral("AI の部品（onnxruntime.dll）を読み込めません（エラー %1）").arg(e);
      return;
    }
    unsigned vcMajor = 0, vcMinor = 0;
    if (moduleVersion(L"msvcp140.dll", &vcMajor, &vcMinor) && (vcMajor < 14 || (vcMajor == 14 && vcMinor < kMinVcMinor))) {
      FreeLibrary(h);
      s.error = oldRuntimeMessage();
      return;
    }
    using GetApiBase = const OrtApiBase*(ORT_API_CALL*)();
    auto getBase = reinterpret_cast<GetApiBase>(reinterpret_cast<void*>(GetProcAddress(h, "OrtGetApiBase")));
    const OrtApiBase* base = getBase ? getBase() : nullptr;
    const OrtApi* api = base ? base->GetApi(ORT_API_VERSION) : nullptr;
    if (!api) {
      s.error = QStringLiteral("AI の部品（onnxruntime.dll）の版が合いません。アプリを入れ直してください");
      return;
    }
    // スレッドの組は、アプリで 1 つ（モデルごとに作ると、同時に動かしたときにコアを取り合う）。
    // 仕事が無いときは待たずに休む（spin しない。すぐに CPU を手放して、ほかの処理や再生を重くしない）
    OrtThreadingOptions* tp = nullptr;
    OrtEnv* env = nullptr;
    OrtStatus* st = api->CreateThreadingOptions(&tp);
    if (!st) st = api->SetGlobalSpinControl(tp, 0);
    if (!st) st = api->SetGlobalIntraOpNumThreads(tp, 0);
    if (!st) st = api->SetGlobalInterOpNumThreads(tp, 1);
    if (!st) st = api->CreateEnvWithGlobalThreadPools(ORT_LOGGING_LEVEL_ERROR, "e-editor", tp, &env);
    if (tp) api->ReleaseThreadingOptions(tp);
    OrtMemoryInfo* cpu = nullptr;
    if (!st) st = api->CreateCpuMemoryInfo(OrtArenaAllocator, OrtMemTypeDefault, &cpu);
    if (st) {
      s.error = QStringLiteral("AI の部品を準備できません: %1").arg(QString::fromUtf8(api->GetErrorMessage(st)));
      api->ReleaseStatus(st);
      if (env) api->ReleaseEnv(env);
      return;
    }
    s.api = api;
    s.env = env;
    s.cpu = cpu;
  });
  g_attempted = true;
  return s;
}

const OrtApi& api() {
  const OrtApi* a = state().api;
  if (!a) throw OrtFailure("ONNX Runtime is not loaded");
  return *a;
}

void check(OrtStatus* st) {
  if (!st) return;
  const std::string msg = api().GetErrorMessage(st);
  api().ReleaseStatus(st);
  throw OrtFailure(msg);
}

size_t elementCount(const std::vector<int64_t>& shape) {
  size_t n = 1;
  for (const int64_t d : shape) n *= size_t(std::max<int64_t>(0, d));
  return n;
}

OrtTensor makeTensor(const void* data, size_t elemSize, const std::vector<int64_t>& shape, ONNXTensorElementDataType type) {
  OrtValue* v = nullptr;
  check(api().CreateTensorWithDataAsOrtValue(state().cpu, const_cast<void*>(data), elementCount(shape) * elemSize, shape.data(), shape.size(), type, &v));
  return OrtTensor(v);
}

}  // namespace

bool loadOrt(QString* error) {
  const OrtState& s = state();
  if (!s.api && error) *error = s.error;
  return s.api != nullptr;
}

bool ortInstalled(QString* error) {
  if (g_attempted.load()) return loadOrt(error);
  if (QFileInfo::exists(ortDllPath())) return true;
  if (error) *error = missingDllMessage();
  return false;
}

OrtTensor::~OrtTensor() {
  if (v_) api().ReleaseValue(v_);
}

OrtTensor& OrtTensor::operator=(OrtTensor&& o) noexcept {
  if (this != &o) {
    if (v_) api().ReleaseValue(v_);
    v_ = o.v_;
    o.v_ = nullptr;
  }
  return *this;
}

const float* OrtTensor::floats() const {
  void* p = nullptr;
  check(api().GetTensorMutableData(v_, &p));
  return static_cast<const float*>(p);
}

std::vector<int64_t> OrtTensor::shape() const {
  OrtTensorTypeAndShapeInfo* info = nullptr;
  check(api().GetTensorTypeAndShape(v_, &info));
  size_t n = 0;
  OrtStatus* st = api().GetDimensionsCount(info, &n);
  std::vector<int64_t> dims(st ? 0 : n);
  if (!st && n > 0) st = api().GetDimensions(info, dims.data(), n);
  api().ReleaseTensorTypeAndShapeInfo(info);
  check(st);
  return dims;
}

size_t OrtTensor::count() const {
  return elementCount(shape());
}

OrtTensor tensorFloat(const float* data, const std::vector<int64_t>& shape) {
  return makeTensor(data, sizeof(float), shape, ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT);
}

OrtTensor tensorInt32(const int32_t* data, const std::vector<int64_t>& shape) {
  return makeTensor(data, sizeof(int32_t), shape, ONNX_TENSOR_ELEMENT_DATA_TYPE_INT32);
}

OrtTensor tensorBool(const bool* data, const std::vector<int64_t>& shape) {
  static_assert(sizeof(bool) == 1, "ONNX の bool は 1 バイト");
  return makeTensor(data, sizeof(bool), shape, ONNX_TENSOR_ELEMENT_DATA_TYPE_BOOL);
}

OrtModel::OrtModel(const QString& path) {
  QString error;
  if (!loadOrt(&error)) throw OrtFailure(error.toStdString());
  const OrtApi& a = api();
  OrtSessionOptions* so = nullptr;
  check(a.CreateSessionOptions(&so));
  OrtStatus* st = a.SetSessionGraphOptimizationLevel(so, ORT_ENABLE_ALL);
  if (!st) st = a.SetSessionExecutionMode(so, ORT_SEQUENTIAL);
  if (!st) st = a.DisablePerSessionThreads(so);  // アプリで 1 つのスレッドの組を使う
  const std::wstring file = QDir::toNativeSeparators(path).toStdWString();
  if (!st) st = a.CreateSession(state().env, file.c_str(), so, &session_);
  a.ReleaseSessionOptions(so);
  check(st);
}

OrtModel::~OrtModel() {
  if (session_) api().ReleaseSession(session_);
}

std::vector<OrtTensor> OrtModel::run(const std::vector<const char*>& inputNames, const std::vector<const OrtTensor*>& inputs,
                                     const std::vector<const char*>& outputNames) const {
  std::vector<const OrtValue*> in;
  in.reserve(inputs.size());
  for (const OrtTensor* t : inputs) in.push_back(t->get());
  std::vector<OrtValue*> out(outputNames.size(), nullptr);
  OrtStatus* st = api().Run(session_, nullptr, inputNames.data(), in.data(), in.size(), outputNames.data(), outputNames.size(), out.data());
  std::vector<OrtTensor> result;
  result.reserve(out.size());
  for (OrtValue* v : out) result.emplace_back(v);  // 失敗したときも、作られた分は手放す
  check(st);
  return result;
}

}  // namespace kv::mask
