/**
 * @file src/steam_detection.cpp
 * @brief Automatic discovery of locally installed Steam games.
 *
 * Sunshine previously required every application (including Steam games) to be added by hand
 * to `apps.json`. This module scans the local Steam installation -- every configured library
 * folder -- and turns each installed game into a launchable candidate using the
 * `steam://rungameid/<appid>` URI, mirroring the approach already used for the built-in
 * "Steam Big Picture" entry. Detected games are only ever returned to the caller; nothing here
 * writes to `apps.json` directly, so existing configurations are never touched.
 */
#include "steam_detection.h"

// standard includes
#include <algorithm>
#include <filesystem>
#include <fstream>
#include <map>
#include <sstream>

// local includes
#include "logging.h"

#ifdef _WIN32
  #include <windows.h>
#endif

namespace fs = std::filesystem;

namespace steam_detection {

  namespace {

    /**
     * @brief A minimal in-memory representation of a Valve Data Format (VDF) node.
     *
     * Steam uses VDF for `libraryfolders.vdf` and per-game `appmanifest_*.acf` files. The format
     * is a simple, KeyValues-style tree of quoted strings; we only need enough of it to pull a
     * handful of keys back out, so this parser is intentionally small rather than a full VDF/KV
     * implementation.
     */
    struct vdf_node_t {
      std::map<std::string, std::string> values;
      std::map<std::string, vdf_node_t> children;
    };

    /**
     * @brief Read the next quoted string token starting at `pos`.
     * @return The unescaped token contents, and advances `pos` past the closing quote.
     */
    std::string
      read_quoted(const std::string &text, size_t &pos) {
      std::string result;
      // Skip to opening quote.
      while (pos < text.size() && text[pos] != '"') {
        ++pos;
      }
      if (pos >= text.size()) {
        return result;
      }
      ++pos;  // consume opening quote
      while (pos < text.size() && text[pos] != '"') {
        if (text[pos] == '\\' && pos + 1 < text.size()) {
          result += text[pos + 1];
          pos += 2;
        } else {
          result += text[pos];
          ++pos;
        }
      }
      ++pos;  // consume closing quote
      return result;
    }

    /**
     * @brief Parse a VDF document body into a node tree.
     * @param text Full file contents.
     * @param pos Current scan position, advanced as the document is consumed.
     * @return The parsed node.
     */
    vdf_node_t
      parse_vdf_node(const std::string &text, size_t &pos) {
      vdf_node_t node;

      while (pos < text.size()) {
        // Skip whitespace and comments.
        while (pos < text.size() && std::isspace(static_cast<unsigned char>(text[pos]))) {
          ++pos;
        }
        if (pos >= text.size()) {
          break;
        }

        if (text[pos] == '}') {
          ++pos;  // consume closing brace, end of this node
          break;
        }

        if (text[pos] != '"') {
          // Unexpected token; skip it to avoid an infinite loop on malformed input.
          ++pos;
          continue;
        }

        std::string key = read_quoted(text, pos);

        // Skip whitespace between key and value/brace.
        while (pos < text.size() && std::isspace(static_cast<unsigned char>(text[pos]))) {
          ++pos;
        }

        if (pos < text.size() && text[pos] == '{') {
          ++pos;  // consume opening brace
          node.children[key] = parse_vdf_node(text, pos);
        } else if (pos < text.size() && text[pos] == '"') {
          node.values[key] = read_quoted(text, pos);
        }
      }

      return node;
    }

    /**
     * @brief Parse a full VDF file's contents.
     */
    vdf_node_t
      parse_vdf(const std::string &text) {
      size_t pos = 0;
      return parse_vdf_node(text, pos);
    }

    std::string
      read_file_contents(const fs::path &path) {
      std::ifstream file(path, std::ios::binary);
      if (!file) {
        return {};
      }
      std::ostringstream ss;
      ss << file.rdbuf();
      return ss.str();
    }

    /**
     * @brief Candidate Steam install locations that don't require reading the registry.
     */
    std::vector<fs::path>
      default_steam_paths() {
      std::vector<fs::path> paths;

#ifdef _WIN32
      if (const char *program_files_x86 = std::getenv("ProgramFiles(x86)")) {
        paths.emplace_back(fs::path(program_files_x86) / "Steam");
      }
      if (const char *program_files = std::getenv("ProgramFiles")) {
        paths.emplace_back(fs::path(program_files) / "Steam");
      }
#else
      if (const char *home = std::getenv("HOME")) {
        paths.emplace_back(fs::path(home) / ".local" / "share" / "Steam");
        paths.emplace_back(fs::path(home) / ".steam" / "steam");
        paths.emplace_back(fs::path(home) / ".steam" / "root");
        paths.emplace_back(fs::path(home) / ".var" / "app" / "com.valvesoftware.Steam" / "data" / "Steam");
      }
#endif

      return paths;
    }

#ifdef _WIN32
    /**
     * @brief Read Steam's install path from the Windows registry, if present.
     */
    std::string
      steam_path_from_registry() {
      for (HKEY root : {HKEY_CURRENT_USER, HKEY_LOCAL_MACHINE}) {
        for (const wchar_t *subkey : {
               L"SOFTWARE\\Valve\\Steam",
               L"SOFTWARE\\WOW6432Node\\Valve\\Steam"
             }) {
          HKEY key;
          if (RegOpenKeyExW(root, subkey, 0, KEY_READ, &key) != ERROR_SUCCESS) {
            continue;
          }

          wchar_t buffer[MAX_PATH] = {};
          DWORD size = sizeof(buffer);
          LONG result = RegQueryValueExW(key, L"InstallPath", nullptr, nullptr, reinterpret_cast<LPBYTE>(buffer), &size);
          if (result != ERROR_SUCCESS) {
            size = sizeof(buffer);
            result = RegQueryValueExW(key, L"SteamPath", nullptr, nullptr, reinterpret_cast<LPBYTE>(buffer), &size);
          }
          RegCloseKey(key);

          if (result == ERROR_SUCCESS) {
            int len = WideCharToMultiByte(CP_UTF8, 0, buffer, -1, nullptr, 0, nullptr, nullptr);
            if (len > 0) {
              std::string path(len - 1, '\0');
              WideCharToMultiByte(CP_UTF8, 0, buffer, -1, path.data(), len, nullptr, nullptr);
              return path;
            }
          }
        }
      }

      return {};
    }
#endif

    /**
     * @brief Parse `libraryfolders.vdf` and return every library path it lists, including the
     *        primary Steam install itself.
     */
    std::vector<fs::path>
      steam_library_paths(const fs::path &steam_path) {
      std::vector<fs::path> libraries {steam_path};

      fs::path library_folders_vdf = steam_path / "steamapps" / "libraryfolders.vdf";
      std::string contents = read_file_contents(library_folders_vdf);
      if (contents.empty()) {
        return libraries;
      }

      vdf_node_t root = parse_vdf(contents);
      auto top = root.children.find("libraryfolders");
      if (top == root.children.end()) {
        return libraries;
      }

      for (auto &[index, entry] : top->second.children) {
        auto path_it = entry.values.find("path");
        if (path_it != entry.values.end()) {
          libraries.emplace_back(fs::u8path(path_it->second));
        }
      }

      return libraries;
    }

  }  // namespace

  std::string
    find_steam_install_path() {
#ifdef _WIN32
    std::string registry_path = steam_path_from_registry();
    if (!registry_path.empty() && fs::exists(registry_path)) {
      return registry_path;
    }
#endif

    for (const fs::path &candidate : default_steam_paths()) {
      std::error_code ec;
      if (fs::exists(candidate, ec)) {
        return candidate.u8string();
      }
    }

    return {};
  }

  std::vector<detected_app_t>
    detect_installed_games() {
    std::vector<detected_app_t> apps;

    std::string steam_path = find_steam_install_path();
    if (steam_path.empty()) {
      BOOST_LOG(info) << "Steam detection: no Steam installation found on this system";
      return apps;
    }

    for (const fs::path &library : steam_library_paths(fs::u8path(steam_path))) {
      std::error_code ec;
      fs::path steamapps_dir = library / "steamapps";
      if (!fs::is_directory(steamapps_dir, ec)) {
        continue;
      }

      for (const auto &entry : fs::directory_iterator(steamapps_dir, ec)) {
        if (ec || !entry.is_regular_file()) {
          continue;
        }

        const fs::path &manifest_path = entry.path();
        std::string filename = manifest_path.filename().u8string();
        if (filename.rfind("appmanifest_", 0) != 0 || manifest_path.extension() != ".acf") {
          continue;
        }

        std::string contents = read_file_contents(manifest_path);
        if (contents.empty()) {
          continue;
        }

        vdf_node_t root = parse_vdf(contents);
        auto state = root.children.find("AppState");
        if (state == root.children.end()) {
          continue;
        }

        auto appid_it = state->second.values.find("appid");
        auto name_it = state->second.values.find("name");
        if (appid_it == state->second.values.end() || name_it == state->second.values.end()) {
          continue;
        }

        detected_app_t app;
        app.app_id = appid_it->second;
        app.name = name_it->second;
        app.cmd = "steam://rungameid/" + app.app_id;

        auto installdir_it = state->second.values.find("installdir");
        if (installdir_it != state->second.values.end()) {
          app.install_dir = (steamapps_dir / "common" / installdir_it->second).u8string();
        }

        apps.push_back(std::move(app));
      }
    }

    std::sort(apps.begin(), apps.end(), [](const detected_app_t &a, const detected_app_t &b) {
      return a.name < b.name;
    });

    return apps;
  }

}  // namespace steam_detection
