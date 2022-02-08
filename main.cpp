
#include <iostream>
#include <memory>
#include <string>

#include <dlfcn.h>

#include <ztd/out_ptr.hpp>

#include <DeckLinkAPIDispatch.cpp>

#include <Processing.NDI.Lib.h>

using namespace std::literals;

constexpr auto width = 1920;
constexpr auto height = 1080;
constexpr auto fps = 25;

constexpr auto bmdColourSpace = bmdFormat8BitYUV;
constexpr auto ndiColourSpace = NDIlib_FourCC_type_UYVY;

struct DeckLinkRelease {
  void operator()(IUnknown *p) {
    if (p != nullptr) {
      p->Release();
    }
  }
};

template <typename T> using DeckLinkPtr = std::unique_ptr<T, DeckLinkRelease>;

template <typename T> auto MakeDeckLinkPtr(T *p) { return DeckLinkPtr<T>{p}; }

using ztd::out_ptr::out_ptr;

template <typename T> auto find_if(auto &&it, auto const &f) {
  auto x = DeckLinkPtr<T>{};
  while (it->Next(out_ptr(x)) == S_OK) {
    if (f(x)) {
      return x;
    }
  }
  return DeckLinkPtr<T>{};
}

class Callback : public IDeckLinkInputCallback {
private:
  DeckLinkPtr<IDeckLinkVideoInputFrame> lastFrame;

  void *dl;
  NDIlib_v5 const *lib;
  NDIlib_send_instance_t sender;

public:
  Callback() {
#if OS == APPLE
    auto dir = "/usr/local/lib/"s;
#elif
    auto dir = std::string{NDILIB_REDIST_FOLDER};
#endif
    auto path = dir + NDILIB_LIBRARY_NAME;

    dl = dlopen(path.c_str(), 0);
    if (dl == 0) {
      std::cerr << "Can't find NDI lib\n";
      std::terminate();
    }
    lib = reinterpret_cast<decltype(&NDIlib_v5_load)>(
        dlsym(dl, "NDIlib_v5_load"))();
    if (lib == nullptr) {
      std::cerr << "Can't find NDI lib\n";
      std::terminate();
    }

    auto send_create = NDIlib_send_create_t{"Decklink", nullptr, false, false};

    sender = lib->send_create(&send_create);
    if (sender == nullptr) {
      std::cerr << "Error creating NDI sender\n";
    }
  }

  Callback(Callback const &) = delete;
  Callback &operator=(Callback const &) = delete;
  Callback(Callback &&) = delete;
  Callback &operator=(Callback &&) = delete;

  ~Callback() {
    lib->send_destroy(sender);
    dlclose(dl);
  }

private:
  auto VideoInputFrameArrived(IDeckLinkVideoInputFrame *videoFrame,
                              IDeckLinkAudioInputPacket *audioPacket)
      -> HRESULT override {
    videoFrame->AddRef();
    auto bmd_frame = MakeDeckLinkPtr(videoFrame);

    void *data;
    bmd_frame->GetBytes(&data);

    auto ndi_frame = NDIlib_video_frame_v2_t(
        bmd_frame->GetWidth(), bmd_frame->GetHeight(), ndiColourSpace, fps, 1,
        16.0f / 9.0f, NDIlib_frame_format_type_progressive, 0,
        static_cast<uint8_t *>(data), bmd_frame->GetRowBytes());

    lib->send_send_video_async_v2(sender, &ndi_frame);

    lastFrame = std::move(bmd_frame);
    return S_OK;
  }

  auto
  VideoInputFormatChanged(BMDVideoInputFormatChangedEvents notificationEvents,
                          IDeckLinkDisplayMode *newDisplayMode,
                          BMDDetectedVideoInputFormatFlags detectedSignalFlags)
      -> HRESULT override {
    return E_FAIL;
  }

  auto QueryInterface(REFIID iid, LPVOID *ppv) -> HRESULT override {
    return E_NOINTERFACE;
  }
  auto AddRef() -> ULONG override { return 0; }
  auto Release() -> ULONG override { return 0; }
};

int main(int, char **) {
  auto deckLinkIterator = MakeDeckLinkPtr(CreateDeckLinkIteratorInstance());
  if (deckLinkIterator == nullptr) {
    return EXIT_FAILURE;
  }

  auto deckLink = DeckLinkPtr<IDeckLink>{};
  if (deckLinkIterator->Next(out_ptr(deckLink)) != S_OK) {
    return EXIT_FAILURE;
  }

  auto deckLinkInput = DeckLinkPtr<IDeckLinkInput>{};
  if (deckLink->QueryInterface(IID_IDeckLinkInput, out_ptr(deckLinkInput)) !=
      S_OK) {
    return EXIT_FAILURE;
  }

  auto displayModeIterator = DeckLinkPtr<IDeckLinkDisplayModeIterator>{};
  if (deckLinkInput->GetDisplayModeIterator(out_ptr(displayModeIterator)) !=
      S_OK) {
    return EXIT_FAILURE;
  }

  auto displayMode =
      find_if<IDeckLinkDisplayMode>(displayModeIterator, [&](auto const &mode) {
        if (mode->GetWidth() != width) {
          return false;
        }
        if (mode->GetHeight() != height) {
          return false;
        }
        auto fpsValue = BMDTimeValue{};
        auto fpsScale = BMDTimeValue{};
        mode->GetFrameRate(&fpsValue, &fpsScale);
        if (fpsValue != fps * fpsScale) {
          return false;
        }
        auto supported = false;
        if (deckLinkInput->DoesSupportVideoMode(
                bmdVideoConnectionUnspecified, mode->GetDisplayMode(),
                bmdColourSpace, bmdNoVideoInputConversion,
                bmdSupportedVideoModeDefault, nullptr, &supported) != S_OK) {
          return false;
        }
        return supported;
      });

  if (deckLinkInput->EnableVideoInput(displayMode->GetDisplayMode(),
                                      bmdColourSpace,
                                      bmdVideoInputFlagDefault) != S_OK) {
    return EXIT_FAILURE;
  }

  auto callback = Callback{};

  if (deckLinkInput->SetCallback(&callback) != S_OK) {
    return EXIT_FAILURE;
  }

  if (deckLinkInput->StartStreams() != S_OK) {
    return EXIT_FAILURE;
  }

  // if (deckLinkInput->StopStreams() != S_OK) { return EXIT_FAILURE; }
}