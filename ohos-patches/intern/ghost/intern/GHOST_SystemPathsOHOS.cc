/* SPDX-FileCopyrightText: 2024 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "GHOST_SystemPathsOHOS.hh"

#include <cstdlib>
#include <string>

/* Blender data root: datafiles + scripts are extracted here by the HAP. */
static const char *ohos_files_root()
{
  return "/data/storage/el2/base/haps/entry/files/blender";
}

const char *GHOST_SystemPathsOHOS::getSystemDir(int /*version*/,
                                                 const char * /*versionstr*/) const
{
  /* BKE_appdir appends /scripts, /datafiles etc. relative to this root. */
  static std::string s;
  s = ohos_files_root();
  return s.c_str();
}

const char *GHOST_SystemPathsOHOS::getUserDir(int /*version*/,
                                               const char *versionstr) const
{
  /* User-writable base. BKE_appdir appends versionstr/{config,scripts,
   * datafiles} to whatever we return here (see appdir.cc:509).
   *
   * $HOME points at /storage/Users/currentUser/Download/<bundle> when
   * the ArkTS side successfully materialized it via the DOWNLOAD
   * picker (see EntryAbility.ensurePublicHomeDirs), otherwise the
   * sandbox files_dir. We layer .config/blender under it, matching
   * the Linux convention BKE_appdir expects — the ArkTS seed writes
   * userpref.blend to the same path so it lines up. */
  static std::string s;
  const char *home = getenv("HOME");
  if (home && *home && versionstr && *versionstr) {
    /* Full versioned path — appdir.cc appends "config" / "scripts" /
     * "datafiles" straight onto this, so we must include versionstr
     * ourselves (Linux backend does the same). */
    s = std::string(home) + "/.config/blender/" + versionstr;
    return s.c_str();
  }
  /* Last-resort fallback if HOME somehow isn't set: sandbox root, so
   * Blender at least starts even without persistent preferences. */
  s = ohos_files_root();
  return s.c_str();
}

const char *GHOST_SystemPathsOHOS::getUserSpecialDir(
    GHOST_TUserSpecialDirTypes /*type*/) const
{
  return nullptr;
}

const char *GHOST_SystemPathsOHOS::getBinaryDir() const
{
  /* HAP native libs live here at runtime. */
  return "/data/storage/el1/bundle/libs/arm64";
}

void GHOST_SystemPathsOHOS::addToSystemRecentFiles(const char * /*filepath*/) const
{
  /* no-op */
}