/**
 * @file src/steam_detection.h
 * @brief Declarations for automatic discovery of locally installed Steam games.
 */
#pragma once

// standard includes
#include <string>
#include <vector>

namespace steam_detection {

  /**
   * @brief A candidate application discovered on the local Steam installation.
   */
  struct detected_app_t {
    std::string name;  ///< Game title, as reported by Steam.
    std::string app_id;  ///< Steam AppID.
    std::string cmd;  ///< Launch command (`steam://rungameid/<appid>`).
    std::string install_dir;  ///< Absolute install directory, when known.
  };

  /**
   * @brief Locate the Steam installation directory for the current user.
   * @return Absolute path to the Steam install directory, or an empty string if Steam isn't found.
   */
  std::string find_steam_install_path();

  /**
   * @brief Scan every configured Steam library and collect the games installed in it.
   * @return List of detected applications, sorted by name.
   */
  std::vector<detected_app_t> detect_installed_games();

}  // namespace steam_detection
