
#include <iostream>
#include <memory>
#include <string>
#include <vector>

#include <ztd/out_ptr.hpp>

#include <Processing.NDI.Lib.h>

#if defined(__unix__) || defined(__unix) ||                                    \
    (defined(__APPLE__) && defined(__MACH__))
#define UNIX
#endif

#if defined(UNIX)
#include <DeckLinkAPIDispatch.cpp>
#elif defined(WIN32)
#include <DeckLinkAPI_i.c>
#include <DeckLinkAPI_i.h>
#endif

#if defined(UNIX)
#include <dlfcn.h>
#elif defined(WIN32)
#include <windows.h>
#endif

#if defined(UNIX)
constexpr auto True = true;
constexpr auto False = false;
#elif defined(WIN32)
constexpr auto True = TRUE;
constexpr auto False = FALSE;
#endif

using namespace std::literals;

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

struct DLString {
#if defined(__linux__)
  char const * data;
#elif defined(__APPLE__) && defined(__MACH__)
  CFStringRef data;
#elif defined(WIN32)
  BSTR data;
#endif

  void print() const {
#if defined(__linux__)
    std::cout << data;
#elif defined(__APPLE__) && defined(__MACH__)
    std::cout << CFStringGetCStringPtr(data, kCFStringEncodingASCII);
#elif defined(WIN32)
    std::wcout << data;
#endif
  }

  ~DLString() {
#if defined(__linux__)
    free(const_cast<char*>(data));
#elif defined(__APPLE__) && defined(__MACH__)
    CFRelease(data);
#elif defined(WIN32)
    SysFreeString(data);
#endif
  }
};

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
  DeckLinkPtr<IDeckLinkDisplayMode> displayMode;

  DeckLinkPtr<IDeckLinkVideoInputFrame> lastFrame;

#if defined(UNIX)
  void *
#elif defined(WIN32)
  HMODULE
#endif
      dl;

  NDIlib_v5 const *lib;
  NDIlib_send_instance_t sender;

public:
  Callback(DeckLinkPtr<IDeckLinkDisplayMode> _displayMode) : displayMode{std::move(_displayMode)} {
#if defined(__APPLE__) && defined(__MACH__)
    auto dir = "/usr/local/lib"s;
#else
    auto dir = std::string{std::getenv(NDILIB_REDIST_FOLDER)};
#endif
    auto path = dir + "/" + NDILIB_LIBRARY_NAME;

#if defined(UNIX)
    dl = dlopen(path.c_str(), 0);
#elif defined(WIN32)
    dl = LoadLibraryA(path.c_str());
#endif
    if (!dl) {
      std::cerr << "Can't find NDI lib, please get it from " << NDILIB_REDIST_URL << '\n';
      std::terminate();
    }
    lib = reinterpret_cast<decltype(&NDIlib_v5_load)>(
#if defined(UNIX)
        dlsym
#elif defined(WIN32)
        GetProcAddress
#endif
        (dl, "NDIlib_v5_load"))();
    if (lib == nullptr) {
      std::cerr << "Can't find NDI symbol\n";
      std::terminate();
    }

    auto send_create = NDIlib_send_create_t{"DeckLink", nullptr, false, false};

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
#if defined(UNIX)
    dlclose(dl);
#elif defined(WIN32)
    FreeLibrary(dl);
#endif
  }

private:
  auto VideoInputFrameArrived(IDeckLinkVideoInputFrame *videoFrame,
                              IDeckLinkAudioInputPacket *audioPacket)
      -> HRESULT override {
    videoFrame->AddRef();
    auto bmd_frame = MakeDeckLinkPtr(videoFrame);

    void *data;
    bmd_frame->GetBytes(&data);

    BMDTimeValue fps_value;
    BMDTimeScale fps_scale;
    displayMode->GetFrameRate(&fps_value, &fps_scale);

    auto format = [&] {
      switch (displayMode->GetFieldDominance()) {
        case bmdUnknownFieldDominance:
          std::cerr << "Unknown field dominance\n";
          std::terminate();
        case bmdLowerFieldFirst:
          std::cerr << "NDI does not support bottom field first formats\n";
          std::terminate();
        case bmdUpperFieldFirst:
          return NDIlib_frame_format_type_interleaved;
        case bmdProgressiveFrame:
          return NDIlib_frame_format_type_progressive;
        case bmdProgressiveSegmentedFrame:
          return NDIlib_frame_format_type_interleaved;
        default:
          std::cerr << "Unknown enum case\n";
          std::terminate();
      }
    }();

    auto ndi_frame = NDIlib_video_frame_v2_t(
        bmd_frame->GetWidth(), bmd_frame->GetHeight(), ndiColourSpace, fps_scale, fps_value,
        0.0f, format, 0,
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
    displayMode = MakeDeckLinkPtr(newDisplayMode);
    return S_OK;
  }

  auto QueryInterface(REFIID iid, LPVOID *ppv) -> HRESULT override {
    return E_NOINTERFACE;
  }
  auto AddRef() -> ULONG override { return 0; }
  auto Release() -> ULONG override { return 0; }
};

int main(int, char **) {
  auto deckLink = [&] {
#if defined(UNIX)
    auto deckLinkIterator = MakeDeckLinkPtr(CreateDeckLinkIteratorInstance());
#elif defined(WIN32)
    if (CoInitialize(nullptr) != S_OK) {
      std::cerr << "Could not initialise COM (Windows)\n";
      std::terminate();
    }
    auto deckLinkIterator = DeckLinkPtr<IDeckLinkIterator>{};
    if (CoCreateInstance(CLSID_CDeckLinkIterator, nullptr, CLSCTX_ALL,
                         IID_IDeckLinkIterator,
                         out_ptr(deckLinkIterator)) != S_OK) {
      std::cerr << "Could not get a DeckLink Iterator (Windows)\n";
      std::terminate();
    }
#endif

    if (deckLinkIterator == nullptr) {
      std::cerr << "Could not get a DeckLink Iterator\n";
      std::terminate();
    }

    auto deckLinks = std::vector<DeckLinkPtr<IDeckLink>>{};
    {
      auto deckLink = DeckLinkPtr<IDeckLink>{};
      while (deckLinkIterator->Next(out_ptr(deckLink)) == S_OK) {
        deckLinks.push_back(std::move(deckLink));
      }
    }
  
    auto deckLinkIndex = [&] {
      if (deckLinks.empty()) {
        std::cerr << "Could not find a DeckLink device\n";
        std::terminate();
      } else {
        std::cout << "DeckLinks:\n";
        {
          auto i = 0;
          for (auto const & deckLink : deckLinks) {
            auto name = DLString{};
            deckLink->GetDisplayName(&name.data);
            std::cout << i++ << ' ';
            name.print();
            std::cout << '\n';
          }
        }
        std::cout << "Please select: ";
        auto i = -1;
        while (i < 0 || i >= deckLinks.size()) {
          std::cin >> i;
        }
        return i;
      }
    }();
  
    return std::move(deckLinks[deckLinkIndex]);
  }();

  {
    auto name = DLString{};
    deckLink->GetDisplayName(&name.data);
    std::cout << "Selected: ";
    name.print();
    std::cout << '\n';
  }

  auto deckLinkInput = DeckLinkPtr<IDeckLinkInput>{};
  if (deckLink->QueryInterface(IID_IDeckLinkInput, out_ptr(deckLinkInput)) !=
      S_OK) {
    std::cerr << "Could not get a DeckLink input\n";
    std::terminate();
  }

  auto displayMode = [&] {
    auto displayModeIterator = DeckLinkPtr<IDeckLinkDisplayModeIterator>{};
    if (deckLinkInput->GetDisplayModeIterator(out_ptr(displayModeIterator)) !=
        S_OK) {
      std::cerr << "Could not get a display mode iterator\n";
      std::terminate();
    }

    auto modes = std::vector<DeckLinkPtr<IDeckLinkDisplayMode>>{};
    {
      auto mode = DeckLinkPtr<IDeckLinkDisplayMode>{};
      while (displayModeIterator->Next(out_ptr(mode)) == S_OK) {
        modes.push_back(std::move(mode));
      }
    }
  
    auto const index = [&] {
      if (modes.empty()) {
        std::cerr << "Could not find any display modes\n";
        std::terminate();
      } else {
        std::cout << "Modes:\n";
        {
          auto i = 0;
          for (auto const & mode : modes) {
            auto name = DLString{};
            mode->GetName(&name.data);
            std::cout << i++ << ' ';
            name.print();
            std::cout << '\n';
          }
        }
        std::cout << "Please select: ";
        auto i = -1;
        while (i < 0 || i >= modes.size()) {
          std::cin >> i;
        }
        return i;
      }
    }();
  
    return std::move(modes[index]);
  }();

  {
    auto name = DLString{};
    displayMode->GetName(&name.data);
    std::cout << "Selected: ";
    name.print();
    std::cout << '\n';
  }

/*
  auto displayMode =
      find_if<IDeckLinkDisplayMode>(displayModeIterator, [&](auto const &mode) {
        if (mode->GetWidth() != width) {
          return False;
        }
        if (mode->GetHeight() != height) {
          return False;
        }
        auto fpsValue = BMDTimeValue{};
        auto fpsScale = BMDTimeValue{};
        mode->GetFrameRate(&fpsValue, &fpsScale);
        if (fpsValue != fps * fpsScale) {
          return False;
        }
        auto supported = False;
        if (deckLinkInput->DoesSupportVideoMode(
                bmdVideoConnectionUnspecified, mode->GetDisplayMode(),
                bmdColourSpace, bmdNoVideoInputConversion,
                bmdSupportedVideoModeDefault, nullptr, &supported) != S_OK) {
          return False;
        }
        return supported;
      });
  if (displayMode == nullptr) {
    std::cerr << "Could not find a matching display mode\n";
    std::terminate();
  }
*/

  if (deckLinkInput->EnableVideoInput(displayMode->GetDisplayMode(),
                                      bmdColourSpace,
                                      bmdVideoInputFlagDefault) != S_OK) {
    std::cerr << "Could not enable video input\n";
    std::terminate();
  }

  auto callback = Callback{std::move(displayMode)};

  if (deckLinkInput->SetCallback(&callback) != S_OK) {
    std::cerr << "Could not set callback\n";
    std::terminate();
  }

  if (deckLinkInput->StartStreams() != S_OK) {
    std::cerr << "Could not start streams\n";
    std::terminate();
  }

  while (true) {
  }

  if (deckLinkInput->StopStreams() != S_OK) {
    return EXIT_FAILURE;
  }
}
