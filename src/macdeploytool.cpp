// This file is part of macdeploytool, a fork of macdeployqt
//
// Copyright (C) 2016 The Qt Company Ltd.
// Copyright (C) 2026 Jonas Kvinge <jonas@jkvinge.net>
//
// SPDX-License-Identifier: LicenseRef-Qt-Commercial OR GPL-3.0-only WITH Qt-GPL-exception-1.0

#include <QtGlobal>

#include <utility>
#include <functional>
#include <vector>
#include <map>

#ifdef Q_OS_MACOS
#  include <CoreFoundation/CoreFoundation.h>
#  include <dlfcn.h>
#endif

#include <QCoreApplication>
#include <QByteArray>
#include <QString>
#include <QStringList>
#include <QSet>
#include <QStack>
#include <QDir>
#include <QDirIterator>
#include <QLibraryInfo>
#include <QStandardPaths>
#include <QRegularExpression>
#include <QFile>
#include <QSaveFile>
#include <QProcess>
#include <QDebug>

#include "logging.h"
#include "macdeploytool.h"

using namespace Qt::StringLiterals;

using std::as_const;
using std::function;
using std::vector;
using std::map;

bool operator==(const FrameworkInfo &a, const FrameworkInfo &b) {

  return ((a.framework_path == b.framework_path) && (a.binary_path == b.binary_path));

}

QDebug operator<<(QDebug debug, const FrameworkInfo &info) {

  debug << "Framework name" << info.framework_name << "\n";
  debug << "Framework directory" << info.framework_directory << "\n";
  debug << "Framework path" << info.framework_path << "\n";
  debug << "Binary directory" << info.binary_directory << "\n";
  debug << "Binary name" << info.binary_name << "\n";
  debug << "Binary path" << info.binary_path << "\n";
  debug << "Version" << info.version << "\n";
  debug << "Install name" << info.install_name << "\n";
  debug << "Deployed install name" << info.deployed_install_name << "\n";
  debug << "Source file Path" << info.source_file_path << "\n";
  debug << "Framework Destination Directory (relative to bundle)" << info.framework_destination_directory << "\n";
  debug << "Binary Destination Directory (relative to bundle)" << info.binary_destination_directory << "\n";

  return debug;

}

inline QDebug operator<<(QDebug debug, const ApplicationBundleInfo &info) {

  debug << "Application bundle path" << info.path << "\n";
  debug << "Binary path" << info.binary_path << "\n";
  debug << "Additional libraries" << info.library_paths << "\n";

  return debug;

}

bool MacDeployTool::CheckRequiredTools(const bool run_strip, const bool run_codesign, const bool create_dmg) {

  struct Tool {
    const QString name;
    const bool required;
  };

  const QList<Tool> tools = {
    { u"otool"_s, true },
    { u"install_name_tool"_s, true },
    { u"strip"_s, run_strip },
    { u"codesign"_s, run_codesign },
    { u"hdiutil"_s, create_dmg },
  };

  bool ok = true;
  for (const Tool &tool : tools) {
    if (!tool.required) {
      continue;
    }
    if (QStandardPaths::findExecutable(tool.name).isEmpty()) {
      LogError() << "Required utility" << tool.name << "was not found in PATH.";
      ok = false;
    }
  }

  return ok;

}

bool MacDeployTool::IsMachOFile(const QString &path) {

  QFile file(path);
  if (!file.open(QIODevice::ReadOnly)) {
    return false;
  }

  quint32 magic = 0;
  if (file.read(reinterpret_cast<char *>(&magic), sizeof(magic)) != qint64(sizeof(magic))) {
    return false;
  }

  switch (magic) {
    case 0xfeedface:  // MH_MAGIC
    case 0xcefaedfe:  // MH_CIGAM
    case 0xfeedfacf:  // MH_MAGIC_64
    case 0xcffaedfe:  // MH_CIGAM_64
    case 0xcafebabe:  // FAT_MAGIC
    case 0xbebafeca:  // FAT_CIGAM
      return true;
    default:
      return false;
  }

}

bool MacDeployTool::CheckRelocatableQt() {

#ifdef Q_OS_MACOS

  Dl_info info;
  if (dladdr(reinterpret_cast<void*>(&QLibraryInfo::version), &info) == 0 || !info.dli_fname) {
    LogError() << "Could not determine the location of the Qt library in use, skipping relocatable Qt check.";
    return false;
  }

  QFile file(QString::fromUtf8(info.dli_fname));
  if (!file.open(QIODevice::ReadOnly)) {
    LogError() << "Could not open" << file.fileName() << "to check whether Qt is relocatable, skipping check.";
    return false;
  }
  const QByteArray data = file.readAll();
  file.close();

  static const QByteArray marker = "qt_prfxpath="_ba;
  const int marker_pos = data.indexOf(marker);
  if (marker_pos < 0) {
    LogWarning() << "Could not find the qt_prfxpath marker in" << file.fileName();
    LogWarning() << "Skipping relocatable Qt check.";
    return true;
  }

  const int path_start = marker_pos + marker.size();
  const QByteArray window = data.mid(path_start, 256);
  const int nul_pos = window.indexOf('\0');
  const QByteArray embedded_prefix_path = nul_pos >= 0 ? window.left(nul_pos) : window;

  if (!embedded_prefix_path.isEmpty()) {
    LogError() << "The Qt installation in use was built with -DFEATURE_relocatable=OFF and has a hardcoded prefix path"
               << QString::fromUtf8(embedded_prefix_path) << "baked into" << file.fileName();
    LogError() << "Such a Qt installation cannot be deployed into an app bundle: Qt will keep looking for its files at the original build-time path instead of inside the bundle.";
    LogError() << "Rebuild Qt with -DFEATURE_relocatable=ON (the default) to use macdeploytool.";
    return false;
  }

#endif  // Q_OS_MACOS

  return true;

}

bool MacDeployTool::CopyFilePrintStatus(const QString &source, const QString &destination) {

  if (QFile::exists(destination)) {
    if (always_owerwrite_enabled_) {
      QFile(destination).remove();
    }
    else {
      qDebug() << "File exists, skip copy:" << destination;
      return false;
    }
  }

  if (QFile::copy(source, destination)) {
    {
      QFile file_dest(destination);
      file_dest.setPermissions(file_dest.permissions() | QFile::WriteOwner | QFile::WriteUser);
    }

    LogNormal() << " copied:" << source;
    LogNormal() << " to" << destination;

    // The source file might not have write permissions set. Set the write permission on the target file to make sure we can use install_name_tool on it later.
    QFile file_dest(destination);
    if (file_dest.permissions() & QFile::WriteOwner) {
      return true;
    }

    if (!file_dest.setPermissions(file_dest.permissions() | QFile::WriteOwner)) {
      LogError() << "Failed to set u+w permissions on target file: " << destination;
      return false;
    }

    return true;
  }
  else {
    LogError() << "file copy failed from" << source;
    LogError() << " to" << destination;
    return false;
  }

}

bool MacDeployTool::LinkFilePrintStatus(const QString &file, const QString &link) {

  if (QFile::exists(link)) {
    if (QFile(link).symLinkTarget().isEmpty()) {
      LogError() << link << "exists but it's a file.";
    }
    else {
      LogNormal() << "Symlink exists, skipping:" << link;
    }
    return false;
  }
  else if (QFile::link(file, link)) {
    LogNormal() << " symlink" << link;
    LogNormal() << " points to" << file;
    return true;
  }
  else {
    LogError() << "failed to symlink" << link;
    LogError() << " to" << file;
    return false;
  }

}

void MacDeployTool::Patch_debugInInfoPlist(const QString &info_plist_path) {

  // Older versions of qmake may have the "_debug" binary as the value for CFBundleExecutable. Remove it.
  QFile file(info_plist_path);
  if (file.open(QIODevice::ReadOnly)) {
    QByteArray contents = file.readAll();
    file.close();
    QSaveFile save_file(info_plist_path);
    bool success = save_file.open(QIODevice::WriteOnly | QIODevice::Truncate);
    if (success) {
      contents.replace("_debug", "");  // Surely there are no legit uses of "_debug" in an Info.plist
      save_file.write(contents);
      success = save_file.commit();
    }
    if (!success) {
      LogError() << "Failed to write Info.plist file" << info_plist_path << save_file.errorString();
    }
  }
  else {
    LogError() << "Failed to read Info.plist file" << info_plist_path << file.errorString();
  }

}

OtoolInfo MacDeployTool::FindDependencyInfo(const QString &binary_path) {

  OtoolInfo toolinfo;
  toolinfo.binary_path = binary_path;

  LogDebug() << "Using otool:";
  LogDebug() << " inspecting" << binary_path;

  QProcess process_otool;
  process_otool.start(u"otool"_s, QStringList() << u"-L"_s << binary_path);
  process_otool.waitForFinished(-1);

  if (process_otool.exitStatus() != QProcess::NormalExit || process_otool.exitCode() != 0) {
    LogError() << process_otool.readAllStandardError();
    return toolinfo;
  }

  static const QRegularExpression regexp(u"^\\t(.+) \\(compatibility version (\\d+\\.\\d+\\.\\d+), current version (\\d+\\.\\d+\\.\\d+)(, weak|, reexport)?\\)$"_s);

  const QString output = QString::fromUtf8(process_otool.readAllStandardOutput());
  QStringList output_lines = output.split(u'\n', Qt::SkipEmptyParts);
  if (output_lines.size() < 2) {
    LogError() << "Could not parse otool output:" << output;
    return toolinfo;
  }

  output_lines.removeFirst();  // Remove line containing the binary path
  if (binary_path.contains(".framework/"_L1) || binary_path.endsWith(".dylib"_L1)) {
    const QRegularExpressionMatch regexp_match = regexp.match(output_lines.constFirst());
    if (regexp_match.hasMatch()) {
      const QString installname = regexp_match.captured(1);
      if (QFileInfo(binary_path).fileName() == QFileInfo(installname).fileName()) {
        toolinfo.install_name = installname;
        toolinfo.compatibility_version = QVersionNumber::fromString(regexp_match.captured(2));
        toolinfo.current_version = QVersionNumber::fromString(regexp_match.captured(3));
        output_lines.removeFirst();
      }
      else {
        toolinfo.install_name = binary_path;
      }
    }
    else {
      LogDebug() << "Could not parse otool output line:" << output_lines.constFirst();
      output_lines.removeFirst();
    }
  }

  for (const QString &output_line : as_const(output_lines)) {
    const QRegularExpressionMatch regexp_match = regexp.match(output_line);
    if (regexp_match.hasMatch()) {
      if (regexp_match.captured(1) == toolinfo.install_name) {
        continue;  // Another arch reference to the same binary
      }
      DylibInfo dylibinfo;
      dylibinfo.binary_path = regexp_match.captured(1);
      dylibinfo.compatibility_version = QVersionNumber::fromString(regexp_match.captured(2));
      dylibinfo.current_version = QVersionNumber::fromString(regexp_match.captured(3));
      toolinfo.dependencies << dylibinfo;
    }
    else {
      LogDebug() << "Could not parse otool output line:" << output_line;
    }
  }

  return toolinfo;

}

FrameworkInfo MacDeployTool::ParseOtoolLibraryLine(const QString &line, const QString &appbundle_path, const QList<QString> &rpaths, const bool use_debug_libs) {

  const QString bundle_framework_directory = u"Contents/Frameworks"_s;

  FrameworkInfo framework_info;
  const QString line_trimmed = line.trimmed();

  if (line_trimmed.isEmpty()) {
    return framework_info;
  }

  // Don't deploy system libraries.
  if (line_trimmed.startsWith("/System/Library/"_L1) || (line_trimmed.startsWith("/usr/lib/"_L1) && line_trimmed.contains("libQt"_L1) == false) || line_trimmed.startsWith("@executable_path"_L1) || line_trimmed.startsWith("@loader_path"_L1)) {
    return framework_info;
  }

  // Resolve rpath relative libraries.
  if (line_trimmed.startsWith("@rpath/"_L1)) {
    QString rpath_relative_path = line_trimmed.mid("@rpath/"_L1.size());
    bool found_inside_bundle = false;
    for (const QString &rpath : rpaths) {
      const QString path = QDir::cleanPath(rpath + u'/' + rpath_relative_path);
      // Skip paths already inside the bundle.
      if (!appbundle_path.isEmpty()) {
        if (QDir::isAbsolutePath(appbundle_path)) {
          if (path.startsWith(QDir::cleanPath(appbundle_path) + u'/')) {
            found_inside_bundle = true;
            continue;
          }
        }
        else {
          if (path.startsWith(QDir::cleanPath(QDir::currentPath() + u'/' + appbundle_path) + u'/')) {
            found_inside_bundle = true;
            continue;
          }
        }
      }
      // Try again with substituted rpath.
      FrameworkInfo resolved_framework_info = ParseOtoolLibraryLine(path, appbundle_path, rpaths, use_debug_libs);
      if (!resolved_framework_info.framework_name.isEmpty() && QFile::exists(resolved_framework_info.framework_path)) {
        resolved_framework_info.rpath_used = rpath;
        resolved_framework_info.install_name = line_trimmed;
        return resolved_framework_info;
      }
    }
    if (!rpaths.isEmpty() && !found_inside_bundle) {
      LogError() << "Cannot resolve rpath" << line_trimmed;
      LogError() << " using" << rpaths;
    }
    return framework_info;
  }

  enum class State {
    QtPath,
    FrameworkName,
    DylibName,
    Version,
    FrameworkBinary,
    End
  };
  State state = State::QtPath;
  int part = 0;
  QString name;
  QString qt_path;
  const QString suffix = use_debug_libs ? u"_debug"_s : QString();

  // Split the line into [Qt-path]/lib/qt[Module].framework/Versions/[Version]/
  const QStringList parts = line_trimmed.split(u'/');
  while (part < parts.count()) {
    const QString current_part = parts.at(part).simplified();
    ++part;
    if (current_part.isEmpty()) {
      continue;
    }

    if (state == State::QtPath) {
      // Check for library name part
      if (part < parts.count() && parts.at(part).contains(".dylib"_L1)) {
        framework_info.framework_directory += u'/' + QString(qt_path + current_part + u'/').simplified();
        state = State::DylibName;
        continue;
      }
      else if (part < parts.count() && parts.at(part).endsWith(".framework"_L1)) {
        framework_info.framework_directory += u'/' + QString(qt_path + "lib/"_L1).simplified();
        state = State::FrameworkName;
        continue;
      }
      else if (line_trimmed.startsWith(u'/') == false) {  // If the line does not contain a full path, the app is using a binary Qt package.
        QStringList parts_copy = parts;
        parts_copy.removeLast();
        for (QString &path : library_search_path_) {
          if (!path.endsWith(u'/')) {
            path += u'/';
          }
          const QString name_in_path = path + parts.join(u'/');
          if (QFile::exists(name_in_path)) {
            framework_info.framework_directory = path + parts_copy.join(u'/');
            break;
          }
        }
        if (current_part.contains(".framework"_L1)) {
          if (framework_info.framework_directory.isEmpty()) {
            framework_info.framework_directory = "/Library/Frameworks/"_L1 + parts_copy.join(u'/');
          }
          if (!framework_info.framework_directory.endsWith(u'/')) {
            framework_info.framework_directory += u'/';
          }
          state = State::FrameworkName;
          --part;
          continue;
        }
        else if (current_part.contains(".dylib"_L1)) {
          if (framework_info.framework_directory.isEmpty()) {
            framework_info.framework_directory = "/usr/lib/"_L1 + parts_copy.join(u'/');
          }
          if (!framework_info.framework_directory.endsWith(u'/')) {
            framework_info.framework_directory += u'/';
          }
          state = State::DylibName;
          --part;
          continue;
        }
      }
      qt_path += (current_part + u'/');
    }
    if (state == State::FrameworkName) {
      // Remove ".framework"
      name = current_part;
      name.chop(".framework"_L1.size());
      framework_info.is_dylib = false;
      framework_info.framework_name = current_part;
      state = State::Version;
      ++part;
      continue;
    }
    if (state == State::DylibName) {
      name = current_part;
      framework_info.is_dylib = true;
      framework_info.framework_name = name;
      framework_info.binary_name = name.contains(suffix) ? name : name.left(name.indexOf(u'.')) + suffix + name.mid(name.indexOf(u'.'));
      framework_info.deployed_install_name = "@executable_path/../Frameworks/"_L1 + framework_info.binary_name;
      framework_info.framework_path = framework_info.framework_directory + framework_info.binary_name;
      framework_info.source_file_path = framework_info.framework_path;
      framework_info.framework_destination_directory = bundle_framework_directory + u'/';
      framework_info.binary_destination_directory = framework_info.framework_destination_directory;
      framework_info.binary_directory = framework_info.framework_directory;
      framework_info.binary_path = framework_info.framework_path;
      state = State::End;
      ++part;
      continue;
    }
    else if (state == State::Version) {
      framework_info.version = current_part;
      framework_info.binary_directory = "Versions/"_L1 + framework_info.version;
      framework_info.framework_path = framework_info.framework_directory + framework_info.framework_name;
      framework_info.framework_destination_directory = bundle_framework_directory + u'/' + framework_info.framework_name;
      framework_info.binary_destination_directory = framework_info.framework_destination_directory + u'/' + framework_info.binary_directory;
      state = State::FrameworkBinary;
    }
    else if (state == State::FrameworkBinary) {
      framework_info.binary_name = current_part.contains(suffix) ? current_part : current_part + suffix;
      framework_info.binary_path = u'/' + framework_info.binary_directory + u'/' + framework_info.binary_name;
      framework_info.deployed_install_name = "@executable_path/../Frameworks/"_L1 + framework_info.framework_name + framework_info.binary_path;
      framework_info.source_file_path = framework_info.framework_path + framework_info.binary_path;
      state = State::End;
    }
    else if (state == State::End) {
      break;
    }
  }

  if (!framework_info.source_file_path.isEmpty() && QFile::exists(framework_info.source_file_path)) {
    framework_info.install_name = FindDependencyInfo(framework_info.source_file_path).install_name;
    if (framework_info.install_name.startsWith("@rpath/"_L1)) {
      framework_info.deployed_install_name = framework_info.install_name;
    }
  }

  return framework_info;

}

QString MacDeployTool::FindAppBinary(const QString &appbundle_path) {

  QString binary_path;

#ifdef Q_OS_MACOS
  CFStringRef bundle_path = appbundle_path.toCFString();
  CFURLRef bundle_url = CFURLCreateWithFileSystemPath(kCFAllocatorDefault, bundle_path, kCFURLPOSIXPathStyle, true);
  CFRelease(bundle_path);
  CFBundleRef bundle = CFBundleCreate(kCFAllocatorDefault, bundle_url);
  if (bundle) {
    CFURLRef executable_url = CFBundleCopyExecutableURL(bundle);
    if (executable_url) {
      CFURLRef absolute_executable_url = CFURLCopyAbsoluteURL(executable_url);
      if (absolute_executable_url) {
        CFStringRef executable_path = CFURLCopyFileSystemPath(absolute_executable_url, kCFURLPOSIXPathStyle);
        if (executable_path) {
          binary_path = QString::fromCFString(executable_path);
          CFRelease(executable_path);
        }
        CFRelease(absolute_executable_url);
      }
      CFRelease(executable_url);
    }
    CFRelease(bundle);
  }
  CFRelease(bundle_url);
#endif  // Q_OS_MACOS

  if (QFile::exists(binary_path)) {
    return binary_path;
  }

  LogError() << "Could not find bundle binary for" << appbundle_path;

  return QString();

}

QStringList MacDeployTool::FindAppFrameworkNames(const QString &appbundle_path) {

  QStringList frameworks;

  // Populate the frameworks list with QtFoo.framework etc, as found in /Contents/Frameworks/
  const QString search_path = appbundle_path + "/Contents/Frameworks/"_L1;
  QDirIterator dir_iterator(search_path, QStringList() << u"*.framework"_s, QDir::Dirs | QDir::NoSymLinks);
  while (dir_iterator.hasNext()) {
    dir_iterator.next();
    frameworks << dir_iterator.fileInfo().fileName();
  }

  return frameworks;

}

QStringList MacDeployTool::FindAppFrameworkPaths(const QString &appbundle_path) {

  const QString search_path = appbundle_path + "/Contents/Frameworks/"_L1;
  QStringList frameworks;
  QDirIterator dir_iterator(search_path, QStringList() << u"*.framework"_s, QDir::Dirs | QDir::NoSymLinks);
  while (dir_iterator.hasNext()) {
    dir_iterator.next();
    frameworks << dir_iterator.fileInfo().filePath();
  }

  return frameworks;

}

QStringList MacDeployTool::FindAppLibraries(const QString &appbundle_path) {

  QStringList libraries;
  QDirIterator dir_iterator(appbundle_path, QStringList() << u"*.dylib"_s << u"*.so"_s, QDir::Files | QDir::NoSymLinks, QDirIterator::Subdirectories);
  while (dir_iterator.hasNext()) {
    dir_iterator.next();
    libraries << dir_iterator.fileInfo().filePath();
  }

  return libraries;

}

QStringList MacDeployTool::FindAppBundleFiles(const QString &appbundle_path, const bool absolute_path) {

  QStringList files;
  QDirIterator dir_iterator(appbundle_path, QStringList() << u"*"_s, QDir::Files, QDirIterator::Subdirectories);
  while (dir_iterator.hasNext()) {
    dir_iterator.next();
    if (dir_iterator.fileInfo().isSymLink()) {
      continue;
    }
    files << (absolute_path ? dir_iterator.fileInfo().absoluteFilePath() : dir_iterator.fileInfo().filePath());
  }

  return files;

}

QString MacDeployTool::FindEntitlementsFile(const QString &path) {

  QDirIterator dir_iterator(path, QStringList() << u"*.entitlements"_s, QDir::Files, QDirIterator::Subdirectories);
  while (dir_iterator.hasNext()) {
    dir_iterator.next();
    if (dir_iterator.fileInfo().isSymLink()) {
      continue;
    }

    // Return the first entitlements file - only one is used for signing anyway
    return dir_iterator.fileInfo().absoluteFilePath();
  }

  return QString();

}

QList<FrameworkInfo> MacDeployTool::GetQtFrameworks(const QList<DylibInfo> &dependencies, const QString &appbundle_path, const QList<QString> &rpaths, const bool use_debug_libs) {

  QList<FrameworkInfo> libraries;
  for (const DylibInfo &dylib_info : dependencies) {
    const FrameworkInfo info = ParseOtoolLibraryLine(dylib_info.binary_path, appbundle_path, rpaths, use_debug_libs);
    if (info.framework_name.isEmpty() == false) {
      LogDebug() << "Adding framework:";
      LogDebug() << info;
      libraries.append(info);
    }
  }

  return libraries;

}

QString MacDeployTool::ResolveDyldPrefix(const QString &path, const QString &loader_path, const QString &executable_path) {

  if (path.startsWith(u'@')) {
    if (path.startsWith("@executable_path/"_L1)) {
      // Path relative to bundle executable dir
      if (QDir::isAbsolutePath(executable_path)) {
        return QDir::cleanPath(QFileInfo(executable_path).path() + path.mid("@executable_path"_L1.size()));
      }
      else {
        return QDir::cleanPath(QDir::currentPath() + u'/' + QFileInfo(executable_path).path() + path.mid("@executable_path"_L1.size()));
      }
    }
    else if (path.startsWith("@loader_path"_L1)) {
      // Path relative to loader dir
      if (QDir::isAbsolutePath(loader_path)) {
        return QDir::cleanPath(QFileInfo(loader_path).path() + path.mid("@loader_path"_L1.size()));
      }
      else {
        return QDir::cleanPath(QDir::currentPath() + u'/' + QFileInfo(loader_path).path() + path.mid("@loader_path"_L1.size()));
      }
    }
    else {
      LogError() << "Unexpected prefix" << path;
    }
  }

  return path;

}

QList<QString> MacDeployTool::GetBinaryRPaths(const QString &path, const bool resolve, QString executable_path) {

  QProcess process_otool;
  process_otool.start(u"otool"_s, QStringList() << u"-l"_s << path);
  process_otool.waitForFinished();

  if (process_otool.exitCode() != 0) {
    LogError() << process_otool.readAllStandardError();
  }

  if (resolve && executable_path.isEmpty()) {
    executable_path = path;
  }

  QList<QString> rpaths;
  const QString output = QString::fromUtf8(process_otool.readAllStandardOutput());
  const QStringList output_lines = output.split(u'\n');
  for (auto it = output_lines.cbegin(), end = output_lines.cend(); it != end; ++it) {
    if (it->contains("cmd LC_RPATH"_L1) && ++it != end && it->contains("cmdsize"_L1) && ++it != end) {
      const QString &rpath_cmd = *it;
      const int path_start = rpath_cmd.indexOf("path "_L1);
      const int path_end = rpath_cmd.indexOf(" ("_L1);
      if (path_start >= 0 && path_end >= 0 && path_start < path_end) {
        const QString rpath = rpath_cmd.mid(path_start + 5, path_end - path_start - 5);
        if (resolve) {
          rpaths << ResolveDyldPrefix(rpath, path, executable_path);
        }
        else {
          rpaths << rpath;
        }
      }
    }
  }

  return rpaths;

}

QList<FrameworkInfo> MacDeployTool::GetQtFrameworks(const QString &path, const QString &appbundle_path, const QList<QString> &rpaths, const bool use_debug_libs) {

  const OtoolInfo info = FindDependencyInfo(path);
  QList<QString> all_rpaths = rpaths + GetBinaryRPaths(path);
  all_rpaths.removeDuplicates();

  return GetQtFrameworks(info.dependencies, appbundle_path, all_rpaths, use_debug_libs);

}

QList<FrameworkInfo> MacDeployTool::GetQtFrameworksForPaths(const QStringList &paths, const QString &appbundle_path, const QList<QString> &rpaths, const bool use_debug_libs) {

  QList<FrameworkInfo> frameworks;
  QSet<QString> existing_frameworks;
  for (const QString &path : paths) {
    const QList<FrameworkInfo> framework_infos = GetQtFrameworks(path, appbundle_path, rpaths, use_debug_libs);
    for (const FrameworkInfo &framework_info : framework_infos) {
      if (!existing_frameworks.contains(framework_info.framework_path)) {  // Avoid duplicates
        existing_frameworks.insert(framework_info.framework_path);
        frameworks << framework_info;
      }
    }
  }

  return frameworks;

}

// Return bundle-local dependencies. (those starting with @executable_path)
QStringList MacDeployTool::GetBinaryDependencies(const QString executable_path, const QString &path, const QList<QString> &additional_binaries_containing_rpaths, const QString &appbundle_path) {

  bool rpaths_loaded = false;
  QList<QString> rpaths;
  QStringList binaries;
  const QList<DylibInfo> dependencies = FindDependencyInfo(path).dependencies;
  for (const DylibInfo &dependency : dependencies) {
    QString trimmed_line = dependency.binary_path;
    if (trimmed_line.startsWith("@executable_path/"_L1)) {
      const QString binary = QDir::cleanPath(executable_path + trimmed_line.mid("@executable_path/"_L1.size()));
      if (binary != path) {
        binaries.append(binary);
      }
    }
    else if (trimmed_line.startsWith("@loader_path/"_L1)) {
      const QString binary = QDir::cleanPath(QFileInfo(path).path() + u'/' + trimmed_line.mid("@loader_path/"_L1.size()));
      if (binary != path) {
        binaries.append(binary);
      }
    }
    else if (trimmed_line.startsWith("@rpath/"_L1)) {
      if (!rpaths_loaded) {
        rpaths = GetBinaryRPaths(path, true, executable_path);
        for (const QString &binary_path : additional_binaries_containing_rpaths) {
          rpaths += GetBinaryRPaths(binary_path, true);
        }
        rpaths.removeDuplicates();
        rpaths_loaded = true;
      }
      QString resolved_binary;
      QString resolved_outside_binary;
      for (const QString &rpath : as_const(rpaths)) {
        const QString binary = QDir::cleanPath(rpath + u'/' + trimmed_line.mid("@rpath/"_L1.size()));
        LogDebug() << "Checking for" << binary;
        if (QFile::exists(binary)) {
          if (!appbundle_path.isEmpty() && binary.startsWith(appbundle_path)) {
            resolved_binary = binary;
            break;
          }
          else if (resolved_outside_binary.isEmpty()) {
            resolved_outside_binary = binary;
          }
        }
      }
      if (resolved_binary.isEmpty()) {
        resolved_binary = resolved_outside_binary;
      }
      if (!resolved_binary.isEmpty()) {
        binaries.append(resolved_binary);
      }
      else if (!rpaths.isEmpty()) {
        LogError() << "Cannot resolve rpath" << trimmed_line;
        LogError() << " using" << rpaths;
      }
    }
  }

  return binaries;

}

// Copies everything _inside_ source_path to destination_path
bool MacDeployTool::RecursiveCopy(const QString &source_path, const QString &destination_path, const QRegularExpression &ignore_reg_exp) {

  const QDir source_dir(source_path);
  if (!source_dir.exists()) {
    return false;
  }
  QDir().mkpath(destination_path);

  LogNormal() << "copy:" << source_path << destination_path;

  const QStringList files = source_dir.entryList(QStringList() << u"*"_s, QDir::Files | QDir::NoDotAndDotDot);
  const bool has_valid_reg_exp = ignore_reg_exp.isValid() && ignore_reg_exp.pattern().length() > 0;
  for (const QString &file : files) {
    if (has_valid_reg_exp && ignore_reg_exp.match(file).hasMatch()) {
      continue;
    }
    const QString file_source_path = source_path + u'/' + file;
    const QString file_destination_path = destination_path + u'/' + file;
    CopyFilePrintStatus(file_source_path, file_destination_path);
  }

  const QStringList subdirs = source_dir.entryList(QStringList() << u"*"_s, QDir::Dirs | QDir::NoDotAndDotDot);
  for (const QString &subdir : subdirs) {
    RecursiveCopy(source_path + u'/' + subdir, destination_path + u'/' + subdir);
  }

  return true;

}

void MacDeployTool::RecursiveCopyAndDeploy(const QString &appbundle_path, const QList<QString> &rpaths, const QString &source_path, const QString &destination_path) {

  QDir().mkpath(destination_path);

  LogNormal() << "copy:" << source_path << destination_path;

  const QDir source_dir(source_path);

  const QStringList files = source_dir.entryList(QStringList() << u"*"_s, QDir::Files | QDir::NoDotAndDotDot);
  for (const QString &file : files) {
    if (file.endsWith("_debug.dylib"_L1)) {
      continue;  // Skip debug versions
    }

    if (file.endsWith(".qrc"_L1)) {
      continue;
    }

    const QString file_source_path = source_path + u'/' + file;

    if (file.endsWith(".dylib"_L1)) {

      // The .dylib destination path:
      const QString file_destination_dir = appbundle_path + u"/Contents/PlugIns/quick/"_s;
      QDir().mkpath(file_destination_dir);
      const QString file_destination_path = file_destination_dir + file;

      // The .dylib symlink destination path:
      const QString link_destination_path = destination_path + u'/' + file;

      // The (relative) link; with a correct number of "../"'s.
      QString link_path = u"PlugIns/quick/"_s + file;
      const int cdup_count = link_destination_path.count(u'/') - appbundle_path.count(u'/');
      for (int i = 0; i < cdup_count - 2; ++i)
        link_path.prepend("../"_L1);

      if (CopyFilePrintStatus(file_source_path, file_destination_path)) {
        LinkFilePrintStatus(link_path, link_destination_path);

        RunStrip(file_destination_path);
        const bool use_debug_libs = false;
        const bool use_loader_path = false;
        QList<FrameworkInfo> frameworks = GetQtFrameworks(file_destination_path, appbundle_path, rpaths, use_debug_libs);
        DeployQtFrameworks(frameworks, appbundle_path, QStringList(file_destination_path), use_debug_libs, use_loader_path);
      }
    }
    else {
      const QString file_destination_path = destination_path + u'/' + file;
      CopyFilePrintStatus(file_source_path, file_destination_path);
    }
  }

  const QStringList subdirs = source_dir.entryList(QStringList() << u"*"_s, QDir::Dirs | QDir::NoDotAndDotDot);
  for (const QString &subdir : subdirs) {
    if (subdir.endsWith(".dSYM"_L1)) {
      continue;
    }

    RecursiveCopyAndDeploy(appbundle_path, rpaths, source_path + u'/' + subdir, destination_path + u'/' + subdir);
  }

}

QString MacDeployTool::CopyDylib(const FrameworkInfo &framework, const QString path) {

  if (!QFile::exists(framework.source_file_path)) {
    LogError() << "no file at" << framework.source_file_path;
    return QString();
  }

  // Construct destination paths. The full path typically looks like MyApp.app/Contents/Frameworks/libfoo.dylib
  const QString dylib_destination_directory = path + u'/' + framework.framework_destination_directory;
  const QString dylib_destination_binary_path = dylib_destination_directory + u'/' + framework.binary_name;

  // Create destination directory
  if (!QDir().mkpath(dylib_destination_directory)) {
    LogError() << "could not create destination directory" << dylib_destination_directory;
    return QString();
  }

  // Return if the dylib has already been deployed
  if (QFileInfo::exists(dylib_destination_binary_path) && !always_owerwrite_enabled_) {
    return dylib_destination_binary_path;
  }

  // Copy dylib binary
  CopyFilePrintStatus(framework.source_file_path, dylib_destination_binary_path);

  return dylib_destination_binary_path;

}

QString MacDeployTool::CopyFramework(const FrameworkInfo &framework, const QString path) {

  if (!QFile::exists(framework.source_file_path)) {
    LogError() << "no file at" << framework.source_file_path;
    return QString();
  }

  // Construct destination paths. The full path typically looks like MyApp.app/Contents/Frameworks/Foo.framework/Versions/5/QtFoo
  const QString framework_destination_directory = path + u'/' + framework.framework_destination_directory;
  const QString framework_binary_destination_directory = framework_destination_directory + u'/' + framework.binary_directory;
  const QString framework_destination_binary_path = framework_binary_destination_directory + u'/' + framework.binary_name;

  // Return if the framework has aleardy been deployed
  if (QDir(framework_destination_directory).exists() && !always_owerwrite_enabled_) {
    return QString();
  }

  // Create destination directory
  if (!QDir().mkpath(framework_binary_destination_directory)) {
    LogError() << "could not create destination directory" << framework_binary_destination_directory;
    return QString();
  }

  // Now copy the framework. Some parts should be left out (headers/, .prl files).
  // Some parts should be included (Resources/, symlink structure).
  // We want this function to make as few assumptions about the framework as possible while at the same time producing a codesign-compatible framework.

  // Copy framework binary
  CopyFilePrintStatus(framework.source_file_path, framework_destination_binary_path);

  // Copy Resources/, Libraries/ and Helpers/
  const QString resources_source_path = framework.framework_path + "/Resources"_L1;
  const QString resources_destination_path = framework_destination_directory + "/Versions/"_L1 + framework.version + "/Resources"_L1;
  // Ignore *.prl files that are in the Resources directory
  RecursiveCopy(resources_source_path, resources_destination_path, QRegularExpression(u"\\A(?:[^/]*\\.prl)\\z"_s));
  const QString libraries_source_path = framework.framework_path + "/Libraries"_L1;
  const QString libraries_destination_path = framework_destination_directory + "/Versions/"_L1 + framework.version + "/Libraries"_L1;
  const bool created_libraries = RecursiveCopy(libraries_source_path, libraries_destination_path);
  const QString helpers_source_path = framework.framework_path + "/Helpers"_L1;
  const QString helpers_destination_path = framework_destination_directory + "/Versions/"_L1 + framework.version + "/Helpers"_L1;
  const bool created_helpers = RecursiveCopy(helpers_source_path, helpers_destination_path);

  // Create symlink structure. Links at the framework root point to Versions/Current/ which again points to the actual version:
  // QtFoo.framework/QtFoo -> Versions/Current/QtFoo
  // QtFoo.framework/Resources -> Versions/Current/Resources
  // QtFoo.framework/Versions/Current -> 5
  LinkFilePrintStatus("Versions/Current/"_L1 + framework.binary_name, framework_destination_directory + "/"_L1 + framework.binary_name);
  LinkFilePrintStatus(u"Versions/Current/Resources"_s, framework_destination_directory + "/Resources"_L1);
  if (created_libraries) {
    LinkFilePrintStatus(u"Versions/Current/Libraries"_s, framework_destination_directory + "/Libraries"_L1);
  }
  if (created_helpers) {
    LinkFilePrintStatus(u"Versions/Current/Helpers"_s, framework_destination_directory + "/Helpers"_L1);
  }
  LinkFilePrintStatus(framework.version, framework_destination_directory + "/Versions/Current"_L1);

  // Correct Info.plist location for frameworks produced by older versions of qmake
  // Contents/Info.plist should be Versions/5/Resources/Info.plist
  const QString legacy_info_plist_path = framework.framework_path + "/Contents/Info.plist"_L1;
  const QString correct_info_plist_path = framework_destination_directory + "/Resources/Info.plist"_L1;
  if (QFile::exists(legacy_info_plist_path)) {
    CopyFilePrintStatus(legacy_info_plist_path, correct_info_plist_path);
    Patch_debugInInfoPlist(correct_info_plist_path);
  }

  return framework_destination_binary_path;

}

void MacDeployTool::RunInstallNameTool(const QStringList &options) {

  QProcess process_install_nametool;
  process_install_nametool.start(u"install_name_tool"_s, options);
  process_install_nametool.waitForFinished();
  if (process_install_nametool.exitCode() != 0) {
    LogError() << process_install_nametool.readAllStandardError();
    LogError() << process_install_nametool.readAllStandardOutput();
  }

}

void MacDeployTool::ChangeIdentification(const QString &id, const QString &binary_path) {

  LogDebug() << "Using install_name_tool:";
  LogDebug() << " change identification in" << binary_path;
  LogDebug() << " to" << id;
  RunInstallNameTool(QStringList() << u"-id"_s << id << binary_path);

}

void MacDeployTool::ChangeInstallName(const QString &bundle_path, const FrameworkInfo &framework, const QStringList &binary_paths, const bool use_loader_path) {

  const QString abs_bundle_path = QFileInfo(bundle_path).absoluteFilePath();
  for (const QString &binary : binary_paths) {
    QString deployed_install_name;
    if (use_loader_path) {
      deployed_install_name = "@loader_path/"_L1 + QFileInfo(binary).absoluteDir().relativeFilePath(abs_bundle_path + u'/' + framework.binary_destination_directory + u'/' + framework.binary_name);
    }
    else {
      deployed_install_name = framework.deployed_install_name;
    }
    ChangeInstallName(framework.install_name, deployed_install_name, binary);
    // Workaround for the case when the library ID name is a symlink, while the dependencies specified using the canonical path to the library (QTBUG-56814)
    const QFileInfo file_info = QFileInfo(framework.install_name);
    const QString canonical_install_name = file_info.canonicalFilePath();
    if (!canonical_install_name.isEmpty() && canonical_install_name != framework.install_name) {
      ChangeInstallName(canonical_install_name, deployed_install_name, binary);
      // Some libraries' inner dependencies (such as ffmpeg, nettle) use symbol link (QTBUG-100093)
      const QString inner_dependency = file_info.canonicalPath() + u'/' + file_info.fileName();
      if (inner_dependency != canonical_install_name && inner_dependency != framework.install_name) {
        ChangeInstallName(inner_dependency, deployed_install_name, binary);
      }
    }
  }

}

void MacDeployTool::AddRPath(const QString &rpath, const QString &binary_path) {

  RunInstallNameTool(QStringList() << u"-add_rpath"_s << rpath << binary_path);

}

void MacDeployTool::DeployRPaths(const QString &bundle_path, const QList<QString> &rpaths, const QString &binary_path, const bool use_loader_path) {

  const QString abs_frameworks_path = QFileInfo(bundle_path).absoluteFilePath() + "/Contents/Frameworks"_L1;
  const QString relative_framework_path = QFileInfo(binary_path).absoluteDir().relativeFilePath(abs_frameworks_path);
  const QString loader_path_to_frameworks = "@loader_path/"_L1 + relative_framework_path;
  bool rpath_to_frameworks_found = false;
  QStringList args;
  const QList<QString> binary_rpaths = GetBinaryRPaths(binary_path, false);
  for (const QString &rpath : binary_rpaths) {
    if (rpath == "@executable_path/../Frameworks"_L1 || rpath == loader_path_to_frameworks) {
      rpath_to_frameworks_found = true;
      continue;
    }
    if (rpaths.contains(ResolveDyldPrefix(rpath, binary_path, binary_path))) {
      if (!args.contains(rpath)) {
        args << u"-delete_rpath"_s << rpath;
      }
    }
  }
  if (!args.length()) {
    return;
  }
  if (!rpath_to_frameworks_found) {
    if (!use_loader_path) {
      args << u"-add_rpath"_s << u"@executable_path/../Frameworks"_s;
    }
    else {
      args << u"-add_rpath"_s << loader_path_to_frameworks;
    }
  }
  LogDebug() << "Using install_name_tool:";
  LogDebug() << " change rpaths in" << binary_path;
  LogDebug() << " using" << args;
  RunInstallNameTool(QStringList() << args << binary_path);

}

void MacDeployTool::DeployRPaths(const QString &bundle_path, const QList<QString> &rpaths, const QStringList &binary_paths, bool use_loader_path) {

  for (const QString &binary : binary_paths) {
    DeployRPaths(bundle_path, rpaths, binary, use_loader_path);
  }

}

void MacDeployTool::ChangeInstallName(const QString &old_name, const QString &new_name, const QString &binary_path) {

  if (new_name == old_name) {
    LogDebug() << "Skipping install_name_tool for" << binary_path << "as name is already" << new_name;
    return;
  }
  LogDebug() << "Using install_name_tool:";
  LogDebug() << " in" << binary_path;
  LogDebug() << " change reference" << old_name;
  LogDebug() << " to" << new_name;
  RunInstallNameTool(QStringList() << u"-change"_s << old_name << new_name << binary_path);

}

void MacDeployTool::RunStrip(const QString &binary_path) {

  if (run_strip_enabled_ == false) {
    return;
  }

  LogDebug() << "Using strip:";
  LogDebug() << " stripped" << binary_path;
  QProcess process_strip;
  process_strip.start(u"strip"_s, QStringList() << u"-x"_s << binary_path);
  process_strip.waitForFinished();
  if (process_strip.exitCode() != 0) {
    LogError() << process_strip.readAllStandardError();
    LogError() << process_strip.readAllStandardOutput();
  }

}

void MacDeployTool::StripAppBinary(const QString &bundle_path) {

  RunStrip(FindAppBinary(bundle_path));

}


// Deploys the listed frameworks into an app bundle.
// The frameworks are searched for dependencies, which are also deployed.
// (deploying Qt3Support will also deploy QtNetwork and QtSql for example.)
// Returns a DeploymentInfo structure containing the Qt path used and a a list of actually deployed frameworks.

DeploymentInfo MacDeployTool::DeployQtFrameworks(QList<FrameworkInfo> frameworks, const QString &bundle_path, const QStringList &binary_paths, const bool use_debug_libs, const bool use_loader_path) {

  LogNormal();
  LogNormal() << "Deploying Qt frameworks found inside:" << binary_paths;

  QStringList copied_frameworks;
  DeploymentInfo deployment_info;
  deployment_info.use_loader_path = use_loader_path;
  deployment_info.is_framework = bundle_path.contains(".framework"_L1);
  deployment_info.is_debug = false;
  QList<QString> rpaths_used;

  while (frameworks.isEmpty() == false) {
    const FrameworkInfo framework_info = frameworks.takeFirst();
    copied_frameworks.append(framework_info.framework_name);

    // If a single dependency has the _debug suffix, we treat that as the whole deployment being a debug deployment, including deploying the debug version of plugins.
    if (framework_info.IsDebugLibrary()) {
      deployment_info.is_debug = true;
    }

    if (deployment_info.qt_path.isNull()) {
      deployment_info.qt_path = QLibraryInfo::paths(QLibraryInfo::PrefixPath).value(0);
    }

    if (framework_info.framework_directory.startsWith(bundle_path)) {
      LogError() << framework_info.framework_name << "already deployed, skipping.";
      continue;
    }

    if (deployment_info.gio_modules_directory.isEmpty() && framework_info.binary_name.startsWith("libgio-2.0"_L1)) {
      const QString candidate = framework_info.framework_directory + "/gio/modules"_L1;
      if (QDir(candidate).exists()) {
        deployment_info.gio_modules_directory = candidate;
      }
    }
    else if (deployment_info.gstreamer_plugins_directory.isEmpty() && framework_info.binary_name.startsWith("libgstreamer-1.0"_L1)) {
      const QString candidate = framework_info.framework_directory + "/gstreamer-1.0"_L1;
      if (QDir(candidate).exists()) {
        deployment_info.gstreamer_plugins_directory = candidate;
      }
    }

    if (!framework_info.rpath_used.isEmpty() && !rpaths_used.contains(framework_info.rpath_used)) {
      rpaths_used.append(framework_info.rpath_used);
    }

    // To properly find all dependencies of the current framework / library further down in GetQtFrameworks, we need to get its rpaths, resolve them in the context of its original location before it is copied, and add them as candidate rpaths.
    //
    // This is necessary to handle cases like
    // (1) QtNetwork.framework -> (2) libbrotlidec.dylib -> (3) libbrotlicommon.1.dylib to correctly resolve the path to (3) when it is referenced as '@rpath/libbrotlicommon.1.dylib' and (2) has an LC_RPATH of '@loader_path/../lib', and no other absolute rpaths. So the '@loader_path/../lib' will be resolved relative to (2)'s original location and its LC_RPATH.
    //
    // Otherwise we'd only have the Qt prefix and the current bundle app dir as rpath candidates, and once (2) is copied into the app bundle, there's no way '@rpath/libbrotlicommon.1.dylib' could resolve to the real path on disk from the two candidates above.
    if (!framework_info.source_file_path.isEmpty()) {
      const QList<QString> source_rpaths = GetBinaryRPaths(framework_info.source_file_path, true);
      for (const QString &source_rpath : source_rpaths) {
        const QDir source_rpath_dir(source_rpath);
        if (source_rpath_dir.exists() && !rpaths_used.contains(source_rpath)) {
          rpaths_used.append(source_rpath);
        }
      }
    }

    // Copy the framework/dylib to the app bundle.
    const QString deployed_binary_path = framework_info.is_dylib ? CopyDylib(framework_info, bundle_path) : CopyFramework(framework_info, bundle_path);

    // Install_name_tool the new id into the binaries
    ChangeInstallName(bundle_path, framework_info, binary_paths, use_loader_path);

    // Skip the rest if already was deployed.
    if (deployed_binary_path.isNull()) {
      continue;
    }

    RunStrip(deployed_binary_path);

    // Install_name_tool it a new id.
    if (!framework_info.rpath_used.length()) {
      ChangeIdentification(framework_info.deployed_install_name, deployed_binary_path);
    }

    DeployRPaths(bundle_path, rpaths_used, deployed_binary_path, use_loader_path);

    // Check for framework dependencies
    const QList<FrameworkInfo> dependencies = GetQtFrameworks(deployed_binary_path, bundle_path, rpaths_used, use_debug_libs);
    for (const FrameworkInfo &dependency : dependencies) {
      if (dependency.rpath_used.isEmpty()) {
        ChangeInstallName(bundle_path, dependency, QStringList() << deployed_binary_path, use_loader_path);
      }
      else if (!rpaths_used.contains(dependency.rpath_used)) {
        rpaths_used.append(dependency.rpath_used);
      }

      // Deploy framework if necessary.
      if (copied_frameworks.contains(dependency.framework_name) == false && frameworks.contains(dependency) == false) {
        frameworks.append(dependency);
      }
    }
  }

  deployment_info.deployed_frameworks = copied_frameworks;
  DeployRPaths(bundle_path, rpaths_used, binary_paths, use_loader_path);
  deployment_info.rpaths_used += rpaths_used;
  deployment_info.rpaths_used.removeDuplicates();

  return deployment_info;

}

DeploymentInfo MacDeployTool::DeployQtFrameworks(const QString &appbundle_path, const QStringList &additional_executables, bool use_debug_libs) {

  ApplicationBundleInfo application_bundle;
  application_bundle.path = appbundle_path;
  application_bundle.binary_path = FindAppBinary(appbundle_path);
  application_bundle.library_paths = FindAppLibraries(appbundle_path);

  const QStringList all_binary_paths = QStringList() << application_bundle.binary_path << application_bundle.library_paths << additional_executables;

  QList<QString> all_library_paths = GetBinaryRPaths(application_bundle.binary_path, true);
  all_library_paths.append(QLibraryInfo::paths(QLibraryInfo::LibrariesPath));
  all_library_paths.removeDuplicates();

  QList<FrameworkInfo> frameworks = GetQtFrameworksForPaths(all_binary_paths, appbundle_path, all_library_paths, use_debug_libs);
  if (frameworks.isEmpty() && !always_owerwrite_enabled_) {
    LogWarning();
    LogWarning() << "Could not find any external Qt frameworks to deploy in" << appbundle_path;
    LogWarning() << "Perhaps macdeploytool was already used on" << appbundle_path << "?";
    LogWarning() << "If so, you will need to rebuild" << appbundle_path << "before trying again.";
    return DeploymentInfo();
  }
  else {
    return DeployQtFrameworks(frameworks, application_bundle.path, all_binary_paths, use_debug_libs, !additional_executables.isEmpty());
  }

}

QString MacDeployTool::GetLibInfix(const QStringList &deployed_frameworks) {

  QString lib_infix;
  for (const QString &framework : deployed_frameworks) {
    if (framework.startsWith("QtCore"_L1) && framework.endsWith(".framework"_L1) && !framework.contains("5Compat"_L1)) {
      Q_ASSERT(framework.length() >= 16);
      // 16 == "QtCore" + ".framework"
      const int length_of_lib_infix = framework.length() - 16;
      if (length_of_lib_infix) {
        lib_infix = framework.mid(6, length_of_lib_infix);
      }
      break;
    }
  }

  return lib_infix;

}

void MacDeployTool::DeployPlugins(const ApplicationBundleInfo &app_bundle_info, const QString &plugin_source_path, const QString plugin_destination_path, DeploymentInfo deployment_info, const bool use_debug_libs) {

  LogNormal() << "Deploying plugins from" << plugin_source_path;

  if (!plugin_source_path.contains(deployment_info.plugin_path)) {
    return;
  }

  // Plugin white list:
  QStringList plugin_list;

  const auto add_plugins = [&plugin_source_path, &plugin_list, use_debug_libs](const QString &sub_directory, const function<bool(QString)> &predicate = function<bool(QString)>()) {
    const QStringList libs = QDir(plugin_source_path + u'/' + sub_directory).entryList({ u"*.dylib"_s });
    for (const QString &lib : libs) {
      if (lib.endsWith("_debug.dylib"_L1) != use_debug_libs) {
        continue;
      }
      if (!predicate || predicate(lib)) {
        plugin_list.append(sub_directory + u'/' + lib);
      }
    }
  };

  // Platform plugin:
  add_plugins(u"platforms"_s, [](const QString &lib) {
    // Ignore minimal and offscreen platform plugins
    if (!lib.contains("cocoa"_L1)) {
      return false;
    }
    return true;
  });

  // Cocoa print support
  add_plugins(u"printsupport"_s);

  // Styles
  add_plugins(u"styles"_s);

  // Check if Qt was configured with -libinfix
  const QString lib_infix = GetLibInfix(deployment_info.deployed_frameworks);

  // Network
  if (deployment_info.ContainsModule(u"Network"_s, lib_infix)) {
    add_plugins(u"tls"_s);
    add_plugins(u"networkinformation"_s);
  }

  // All image formats (svg if QtSvg is used)
  const bool uses_svg = deployment_info.ContainsModule(u"Svg"_s, lib_infix);
  add_plugins(u"imageformats"_s, [uses_svg](const QString &lib) {
    if (lib.contains("qsvg"_L1) && !uses_svg) {
      return false;
    }
    return true;
  });

  add_plugins(u"iconengines"_s);

  // Platforminputcontext plugins if QtGui is in use
  if (deployment_info.ContainsModule(u"Gui"_s, lib_infix)) {
    add_plugins(u"platforminputcontexts"_s, [&add_plugins](const QString &lib) {
      // Deploy the virtual keyboard plugins if we have deployed virtualkeyboard
      if (lib.startsWith("libqtvirtualkeyboard"_L1)) {
        add_plugins(u"virtualkeyboard"_s);
      }
      return true;
    });
  }

  // Sql plugins if QtSql is in use
  if (deployment_info.ContainsModule(u"Sql"_s, lib_infix)) {
    add_plugins(u"sqldrivers"_s, [this](const QString &lib) {
      if (lib.startsWith("libqsqlodbc"_L1) || lib.startsWith("libqsqlpsql"_L1)) {
        LogWarning() << "Plugin" << lib << "uses private API and is not Mac App store compliant.";
        if (appstore_compliant_) {
          LogWarning() << "Skip plugin" << lib;
          return false;
        }
      }
      return true;
    });
  }

  // WebView plugins if QtWebView is in use
  if (deployment_info.ContainsModule(u"WebView"_s, lib_infix)) {
    add_plugins(u"webview"_s, [this](const QString &lib) {
      if (lib.startsWith("libqtwebview_webengine"_L1)) {
        LogWarning() << "Plugin" << lib << "uses QtWebEngine and is not Mac App store compliant.";
        if (appstore_compliant_) {
          LogWarning() << "Skip plugin" << lib;
          return false;
        }
      }
      return true;
    });
  }

  // FIXME: Parse modules/Foo.json's plugin_types instead
  static const map<QString, vector<QString>> map {
    { u"Multimedia"_s, { u"multimedia"_s } },
    { u"3DRender"_s, { u"sceneparsers"_s, u"geometryloaders"_s, u"renderers"_s } },
    { u"3DQuickRender"_s, { u"renderplugins"_s } },
    { u"Positioning"_s, { u"position"_s } },
    { u"Location"_s, { u"geoservices"_s } },
    { u"TextToSpeech"_s, { u"texttospeech"_s } },
    { u"SerialBus"_s, { u"canbus"_s } },
  };

  for (const auto &it : map) {
    if (deployment_info.ContainsModule(it.first, lib_infix)) {
      for (const QString &plugin_type : it.second) {
        add_plugins(plugin_type);
      }
    }
  }

  for (const QString &plugin : as_const(plugin_list)) {
    const QString source_path = plugin_source_path + u'/' + plugin;
    const QString destination_path = plugin_destination_path + u'/' + plugin;
    QDir dir;
    dir.mkpath(QFileInfo(destination_path).path());

    if (CopyFilePrintStatus(source_path, destination_path)) {
      RunStrip(destination_path);
      QList<FrameworkInfo> frameworks = GetQtFrameworks(destination_path, app_bundle_info.path, deployment_info.rpaths_used, use_debug_libs);
      DeployQtFrameworks(frameworks, app_bundle_info.path, QStringList() << destination_path, use_debug_libs, deployment_info.use_loader_path);
    }
  }

}

void MacDeployTool::DeployPlugins(const QString &appbundle_path, DeploymentInfo deployment_info, const bool use_debug_libs) {

  ApplicationBundleInfo application_bundle;
  application_bundle.path = appbundle_path;
  application_bundle.binary_path = FindAppBinary(appbundle_path);

  const QString plugin_destination_path = appbundle_path + "/Contents/PlugIns"_L1;
  DeployPlugins(application_bundle, deployment_info.plugin_path, plugin_destination_path, deployment_info, use_debug_libs);

}

QString MacDeployTool::PkgConfigVariable(const QString &package, const QString &variable) {

  const QString executable = qEnvironmentVariable("PKG_CONFIG_EXECUTABLE", u"pkg-config"_s);
  if (QStandardPaths::findExecutable(executable).isEmpty()) {
    return QString();
  }

  QProcess process_pkg_config;
  process_pkg_config.start(executable, QStringList() << u"--variable=%1"_s.arg(variable) << package);
  if (!process_pkg_config.waitForFinished(5000) || process_pkg_config.exitCode() != 0) {
    return QString();
  }

  return QString::fromUtf8(process_pkg_config.readAllStandardOutput()).trimmed();

}

QString MacDeployTool::FindGioModulesDir(const QString &gio_modules_directory) {

  const QString from_env = qEnvironmentVariable("GIO_EXTRA_MODULES");
  if (!from_env.isEmpty() && QDir(from_env).exists()) {
    return from_env;
  }

  const QString from_pkg_config = PkgConfigVariable(u"gio-2.0"_s, u"libdir"_s);
  if (!from_pkg_config.isEmpty()) {
    const QString candidate = from_pkg_config + "/gio/modules"_L1;
    if (QDir(candidate).exists()) {
      return candidate;
    }
  }

  return gio_modules_directory;

}

void MacDeployTool::DeployGioModules(const QString &appbundle_path, const DeploymentInfo &deployment_info, const bool use_debug_libs) {

  bool uses_gio = false;
  const QStringList libs = FindAppLibraries(appbundle_path);
  for (const QString &lib : libs) {
    const QFileInfo fileinfo(lib);
    if (fileinfo.fileName().startsWith(u"libgio-2.0"_s)) {
      uses_gio = true;
      break;
    }
  }
  if (!uses_gio) {
    return;
  }

  LogNormal() << "";
  LogNormal() << "Deploying GIO modules";

  const QString modules_dir = FindGioModulesDir(deployment_info.gio_modules_directory);
  if (modules_dir.isEmpty()) {
    LogError() << "Could not find the GIO modules directory";
    return;
  }

  const QString destination_dir = appbundle_path + "/Contents/PlugIns/gio/modules"_L1;
  QDir().mkpath(destination_dir);

  const QStringList module_files = QDir(modules_dir).entryList(QStringList() << u"*.dylib"_s << u"*.so"_s, QDir::Files);
  for (const QString &module_file : module_files) {
    const QString source_path = modules_dir + u'/' + module_file;
    const QString destination_path = destination_dir + u'/' + module_file;
    if (CopyFilePrintStatus(source_path, destination_path)) {
      RunStrip(destination_path);
      ChangeIdentification(u"@rpath/"_s + QFileInfo(destination_path).fileName(), destination_path);
      const QList<FrameworkInfo> frameworks = GetQtFrameworks(destination_path, appbundle_path, deployment_info.rpaths_used, use_debug_libs);
      DeployQtFrameworks(frameworks, appbundle_path, QStringList() << destination_path, use_debug_libs, deployment_info.use_loader_path);
    }
  }

  const QString cache = modules_dir + "/giomodule.cache"_L1;
  if (QFile::exists(cache)) {
    CopyFilePrintStatus(cache, destination_dir + "/giomodule.cache"_L1);
  }

}

QString MacDeployTool::FindGStreamerPluginsDir(const QString &gstreamer_plugins_directory) {

  const QString from_env = qEnvironmentVariable("GST_PLUGIN_PATH");
  if (!from_env.isEmpty()) {
    const QStringList paths = from_env.split(QDir::listSeparator(), Qt::SkipEmptyParts);
    for (const QString &path : paths) {
      if (QDir(path).exists()) {
        return path;
      }
    }
  }

  const QString from_pkg_config = PkgConfigVariable(u"gstreamer-1.0"_s, u"pluginsdir"_s);
  if (!from_pkg_config.isEmpty() && QDir(from_pkg_config).exists()) {
    return from_pkg_config;
  }

  return gstreamer_plugins_directory;

}

QString MacDeployTool::FindGstPluginScanner(const QString &plugins_dir) {

  const QString from_env = qEnvironmentVariable("GST_PLUGIN_SCANNER");
  if (!from_env.isEmpty() && QFile::exists(from_env)) {
    return from_env;
  }

  const QString from_pkg_config = PkgConfigVariable(u"gstreamer-1.0"_s, u"pluginscannerdir"_s);
  if (!from_pkg_config.isEmpty()) {
    const QString path = from_pkg_config + "/gst-plugin-scanner"_L1;
    if (QFile::exists(path)) {
      return path;
    }
  }

  if (!plugins_dir.isEmpty()) {
    const QString path = plugins_dir + "/gst-plugin-scanner"_L1;
    if (QFile::exists(path)) {
      return path;
    }
  }

  return QString();

}

QString MacDeployTool::ResolvePluginFile(const QString &plugin_name) {

  QProcess process_gst_inspect;
  process_gst_inspect.start(u"gst-inspect-1.0"_s, QStringList() << plugin_name << u"--no-colors"_s);
  if (!process_gst_inspect.waitForFinished(15000) || process_gst_inspect.exitCode() != 0) {
    return QString();
  }

  const QStringList lines = QString::fromUtf8(process_gst_inspect.readAllStandardOutput()).split(u'\n');
  for (const QString &line : lines) {
    const int marker = line.indexOf("Filename"_L1);
    if (marker < 0) {
      continue;
    }
    const QString path = line.mid(marker + int(u"Filename"_s.size())).trimmed();
    if (!path.isEmpty()) {
      return path;
    }
  }

  return QString();

}

QStringList MacDeployTool::SelectGStreamerPluginFiles(const QString &plugins_dir, GStreamerPluginSet plugin_set, const QStringList &plugin_names) {

  if (plugin_set == GStreamerPluginSet::All) {
    QStringList result;
    QDirIterator dir_iterator(plugins_dir, QStringList() << u"*.dylib"_s << u"*.so"_s, QDir::Files);
    while (dir_iterator.hasNext()) {
      result << dir_iterator.next();
    }
    return result;
  }

  QStringList result;
  for (const QString &plugin_name : plugin_names) {
    const QString file = ResolvePluginFile(plugin_name);
    if (file.isEmpty()) {
      LogError() << "Could not find GStreamer plugin named" << plugin_name;
      continue;
    }
    if (!result.contains(file)) {
      result << file;
    }
  }
  return result;

}

void MacDeployTool::DeployBinaryAndItsDependencies(const QString &appbundle_path, const DeploymentInfo &deployment_info, bool use_debug_libs, const QString &source_path, const QString &destination_dir) {

  const QString destination_path = destination_dir + u'/' + QFileInfo(source_path).fileName();
  if (!CopyFilePrintStatus(source_path, destination_path)) {
    return;
  }

  RunStrip(destination_path);
  ChangeIdentification(u"@rpath/"_s + QFileInfo(destination_path).fileName(), destination_path);

  QList<QString> rpaths_used = deployment_info.rpaths_used;
  const QStringList binary_rpaths = GetBinaryRPaths(source_path, true);
  for (const QString &source_rpath : binary_rpaths) {
    if (QDir(source_rpath).exists() && !rpaths_used.contains(source_rpath)) {
      rpaths_used.append(source_rpath);
    }
  }
  DeployRPaths(appbundle_path, rpaths_used, destination_path, deployment_info.use_loader_path);

  const QList<FrameworkInfo> frameworks = GetQtFrameworks(destination_path, appbundle_path, rpaths_used, use_debug_libs);
  DeployQtFrameworks(frameworks, appbundle_path, QStringList() << destination_path, use_debug_libs, deployment_info.use_loader_path);

}

QString MacDeployTool::FindLibsoupLibrary() {

  const QString from_env = qEnvironmentVariable("LIBSOUP_LIBRARY_PATH");
  if (!from_env.isEmpty()) {
    if (QFile::exists(from_env)) {
      return from_env;
    }
    LogError() << "LIBSOUP_LIBRARY_PATH is set to" << from_env << "but that file does not exist";
  }

  static const struct {
    const QString pkg_config_name;
    const QString soname;
  } names[] = {
    { u"libsoup-3.0"_s, u"libsoup-3.0.0.dylib"_s },
    { u"libsoup-2.4"_s, u"libsoup-2.4.1.dylib"_s },
  };
  for (const auto &name : names) {
    const QString libdir = PkgConfigVariable(name.pkg_config_name, u"libdir"_s);
    if (libdir.isEmpty()) {
      continue;
    }
    const QString path = libdir + u'/' + name.soname;
    if (QFile::exists(path)) {
      return path;
    }
  }

  return QString();

}

bool MacDeployTool::DeployGStreamerPlugins(const QString &appbundle_path, const DeploymentInfo &deployment_info, GStreamerPluginSet plugin_set, const QStringList &plugin_names, bool use_debug_libs) {

  if (plugin_set == GStreamerPluginSet::None) {
    return true;
  }

  bool uses_gstreamer = false;
  const QStringList libs = FindAppLibraries(appbundle_path);
  for (const QString &lib : libs) {
    const QFileInfo fileinfo(lib);
    if (fileinfo.fileName().startsWith("libgstreamer-1.0"_L1)) {
      uses_gstreamer = true;
      break;
    }
  }
  if (!uses_gstreamer) {
    return true;
  }

  if (plugin_set == GStreamerPluginSet::List && plugin_names.isEmpty()) {
    LogError() << "-gstreamer-plugins=list requires -gstreamer-plugin-list=<comma-separated names>";
    return false;
  }

  if (plugin_set == GStreamerPluginSet::List && QStandardPaths::findExecutable(u"gst-inspect-1.0"_s).isEmpty()) {
    LogError() << "gst-inspect-1.0 is required to select specific GStreamer plugins (use -gstreamer-plugins=all to bundle everything without it)";
    return false;
  }

  LogNormal() << "";
  LogNormal() << "Deploying GStreamer plugins";

  const QString plugins_dir = FindGStreamerPluginsDir(deployment_info.gstreamer_plugins_directory);
  if (plugins_dir.isEmpty()) {
    LogError() << "Could not find the GStreamer 1.0 plugins directory";
    return false;
  }

  const QStringList plugin_files = SelectGStreamerPluginFiles(plugins_dir, plugin_set, plugin_names);
  if (plugin_files.isEmpty()) {
    LogError() << "No GStreamer plugins were selected for bundling";
    return true;
  }

  const QString destination_dir = appbundle_path + "/Contents/PlugIns/gstreamer"_L1;
  QDir().mkpath(destination_dir);

  LogNormal() << "Bundling" << plugin_files.size() << "GStreamer plugin(s)...";
  bool needs_libsoup = false;
  for (const QString &plugin_file : plugin_files) {
    DeployBinaryAndItsDependencies(appbundle_path, deployment_info, use_debug_libs, plugin_file, destination_dir);
    if (QFileInfo(plugin_file).fileName().startsWith("libgstsoup"_L1)) {
      needs_libsoup = true;
    }
  }

  if (needs_libsoup) {
    const QString libsoup = FindLibsoupLibrary();
    if (libsoup.isEmpty()) {
      LogError() << "libgstsoup is bundled but libsoup could not be located to deploy alongside it;"
                   << "GStreamer's soup elements will fail to load at runtime";
    }
    else {
      LogNormal() << "Deploying" << libsoup << "for libgstsoup (dlopen'd at runtime, not a link-time dependency)";
      DeployBinaryAndItsDependencies(appbundle_path, deployment_info, use_debug_libs, libsoup, appbundle_path + "/Contents/Frameworks"_L1);
    }
  }

  LogNormal() << "Determining gst-plugin-scanner...";
  const QString scanner = FindGstPluginScanner(plugins_dir);
  if (scanner.isEmpty()) {
    LogError() << "Could not find gst-plugin-scanner; external GStreamer plugins may fail to load";
  }
  else {
    DeployBinaryAndItsDependencies(appbundle_path, deployment_info, use_debug_libs, scanner, appbundle_path + "/Contents/PlugIns"_L1);
  }

  return true;

}

void MacDeployTool::CodesignFile(const QString &identity, const QString &file_path) {

  if (!run_codesign_) {
    return;
  }

  QString code_sign_log_message = u"codesign"_s;
  if (hardened_runtime_) {
    code_sign_log_message += u", enable hardened runtime"_s;
  }
  if (secure_timestamp_) {
    code_sign_log_message += u", include secure timestamp"_s;
  }
  LogNormal() << code_sign_log_message << file_path;

  QStringList code_sign_options = { u"--preserve-metadata=identifier,entitlements"_s, u"--force"_s, u"-s"_s, identity, file_path };
  if (hardened_runtime_) {
    code_sign_options << u"-o"_s << u"runtime"_s;
  }

  if (secure_timestamp_) {
    code_sign_options << u"--timestamp"_s;
  }

  if (!extra_entitlements_.isEmpty()) {
    code_sign_options << u"--entitlements"_s << extra_entitlements_;
  }

  QProcess process_codesign;
  process_codesign.start(u"codesign"_s, code_sign_options);
  process_codesign.waitForFinished(-1);

  QByteArray error = process_codesign.readAllStandardError();
  if (process_codesign.exitCode() > 0) {
    LogError() << "Codesign signing error:";
    LogError() << error;
  }
  else if (!error.isEmpty()) {
    LogDebug() << error;
  }

}

QSet<QString> MacDeployTool::CodesignBundle(const QString &identity, const QString &appbundle_path, QList<QString> additional_binaries_containing_rpaths) {

  // Code sign all binaries in the app bundle. This needs to be done inside-out, e.g sign framework dependencies before the main app binary.
  // The codesign tool itself has a "--deep" option to do this, but usage when signing is not recommended: "Signing with --deep is for emergency repairs and temporary adjustments only."

  LogNormal() << "";
  LogNormal() << "Signing" << appbundle_path << "with identity" << identity;

  QStack<QString> pending_binaries;
  QSet<QString> pending_binaries_set;
  QSet<QString> signed_binaries;

  // Create the root code-binary set. This set consists of the application executable(s) and the plugins.
  const QString app_bundle_absolute_path = QFileInfo(appbundle_path).absoluteFilePath();
  const QString root_binaries_path = app_bundle_absolute_path + "/Contents/MacOS/"_L1;
  QStringList found_root_binaries = QDir(root_binaries_path).entryList(QStringList() << u"*"_s, QDir::Files);

  // The app binary must be signed last.
  QString app_binary = FindAppBinary(app_bundle_absolute_path);
  QString app_binary_name = QFileInfo(app_binary).fileName();
  if (int app_binary_idx = found_root_binaries.indexOf(app_binary_name); app_binary_idx > 0) {
    found_root_binaries.swapItemsAt(0, app_binary_idx);
    LogDebug() << "swapped app_binary to start of list";
  }
  LogDebug() << "App binary is" << app_binary_name;
  LogDebug() << "Binaries in" << root_binaries_path << "are" << found_root_binaries;

  for (const QString &binary : as_const(found_root_binaries)) {
    const QString binary_path = root_binaries_path + binary;
    pending_binaries.push(binary_path);
    pending_binaries_set.insert(binary_path);
    additional_binaries_containing_rpaths.append(binary_path);
  }

  bool get_absoltue_path = true;
  const QStringList found_plugin_binaries = FindAppBundleFiles(appbundle_path + "/Contents/PlugIns/"_L1, get_absoltue_path);
  for (const QString &binary : found_plugin_binaries) {
    pending_binaries.push(binary);
    pending_binaries_set.insert(binary);
  }

  const QString frameworks_path = app_bundle_absolute_path + "/Contents/Frameworks/"_L1;
  const QStringList found_frameworks_dylibs = QDir(frameworks_path).entryList(QStringList() << u"*.dylib"_s, QDir::Files);
  for (const QString &dylib : found_frameworks_dylibs) {
    QString dylib_path = frameworks_path + dylib;
    pending_binaries.push(dylib_path);
    pending_binaries_set.insert(dylib_path);
  }

  // Add frameworks for processing.
  const QStringList framework_paths = FindAppFrameworkPaths(appbundle_path);
  for (const QString &framework_path : framework_paths) {

    // Prioritise first to sign any additional inner bundles found in the Helpers folder (e.g used by QtWebEngine).
    QDirIterator helpers_iterator(framework_path, QStringList() << u"Helpers"_s, QDir::Dirs | QDir::NoSymLinks, QDirIterator::Subdirectories);
    while (helpers_iterator.hasNext()) {
      helpers_iterator.next();
      QString helpers_path = helpers_iterator.filePath();
      const QStringList inner_bundle_names = QDir(helpers_path).entryList(QStringList() << u"*.app"_s, QDir::Dirs);
      for (const QString &inner_bundle_name : inner_bundle_names) {
        signed_binaries += CodesignBundle(identity, helpers_path + u'/' + inner_bundle_name, additional_binaries_containing_rpaths);
      }
    }

    // Also make sure to sign any libraries that will not be found by otool because they are not linked and won't be seen as a dependency.
    QDirIterator libraries_iterator(framework_path, QStringList() << u"Libraries"_s, QDir::Dirs | QDir::NoSymLinks, QDirIterator::Subdirectories);
    while (libraries_iterator.hasNext()) {
      libraries_iterator.next();
      const QString libraries_path = libraries_iterator.filePath();
      const QStringList bundle_files = FindAppBundleFiles(libraries_path, get_absoltue_path);
      for (const QString &binary : bundle_files) {
        pending_binaries.push(binary);
        pending_binaries_set.insert(binary);
      }
    }
  }

  // Sign all binaries; use otool to find and sign dependencies first.
  while (!pending_binaries.isEmpty()) {
    const QString binary = pending_binaries.pop();
    if (signed_binaries.contains(binary)) {
      continue;
    }

    // Check if there are unsigned dependencies, sign these first.
    QStringList dependencies = IsMachOFile(binary) ? GetBinaryDependencies(root_binaries_path, binary, additional_binaries_containing_rpaths, app_bundle_absolute_path) : QStringList();
    dependencies = QSet<QString>(dependencies.begin(), dependencies.end()).subtract(signed_binaries).subtract(pending_binaries_set).values();

    if (!dependencies.isEmpty()) {
      pending_binaries.push(binary);
      pending_binaries_set.insert(binary);
      int dependencies_skipped = 0;
      for (const QString &dependency : as_const(dependencies)) {
        // Skip dependencies that are outside the current app bundle, because this might cause a codesign error if the current bundle is part of the dependency
        // (e.g. a bundle is part of a framework helper, and depends on that framework).
        // The dependencies will be taken care of after the current bundle is signed.
        if (!dependency.startsWith(app_bundle_absolute_path)) {
          ++dependencies_skipped;
          LogNormal() << "Skipping outside dependency: " << dependency;
          continue;
        }
        pending_binaries.push(dependency);
        pending_binaries_set.insert(dependency);
      }

      // If all dependencies were skipped, make sure the binary is actually signed, instead of going into an infinite loop.
      if (dependencies_skipped == dependencies.size()) {
        pending_binaries.pop();
      }
      else {
        continue;
      }
    }

    // Look for an entitlements file in the bundle to include when signing
    extra_entitlements_ = FindEntitlementsFile(app_bundle_absolute_path + "/Contents/Resources/"_L1);

    // All dependencies are signed, now sign this binary.
    CodesignFile(identity, binary);
    signed_binaries.insert(binary);
    pending_binaries_set.remove(binary);
  }

  LogNormal() << "Finished codesigning " << appbundle_path << "with identity" << identity;

  // Verify code signature
  QProcess process_codesign;
  process_codesign.start(u"codesign"_s, QStringList() << u"--deep"_s << u"-v"_s << appbundle_path);
  process_codesign.waitForFinished(-1);
  QByteArray err = process_codesign.readAllStandardError();
  if (process_codesign.exitCode() > 0) {
    LogError() << "codesign verification error:";
    LogError() << err;
  }
  else if (!err.isEmpty()) {
    LogDebug() << err;
  }

  return signed_binaries;

}

void MacDeployTool::Codesign(const QString &identity, const QString &appbundle_path) {

  CodesignBundle(identity, appbundle_path, QList<QString>());

}

void MacDeployTool::CreateDiskImage(const QString &appbundle_path, const QString &filesystem_type) {

  QString app_base_name = appbundle_path;
  app_base_name.chop(4);  // remove ".app" from end

  const QString dmg_name = app_base_name + ".dmg"_L1;

  QFile file_dmg(dmg_name);

  if (file_dmg.exists() && always_owerwrite_enabled_) {
    file_dmg.remove();
  }

  if (file_dmg.exists()) {
    LogNormal() << "Disk image already exists, skipping .dmg creation for" << file_dmg.fileName();
  }
  else {
    LogNormal() << "Creating disk image (.dmg) for" << appbundle_path;
  }

  LogNormal() << "Image will use" << filesystem_type;

  // More dmg options can be found in the hdiutil man page.
  const QStringList options = QStringList()
    << u"create"_s << dmg_name
    << u"-srcfolder"_s << appbundle_path
    << u"-format"_s << u"UDZO"_s
    << u"-fs"_s << filesystem_type
    << u"-volname"_s << app_base_name;

  QProcess process_hdutil;
  process_hdutil.start(u"hdiutil"_s, options);
  process_hdutil.waitForFinished(-1);
  if (process_hdutil.exitCode() != 0) {
    LogError() << "Bundle creation error:" << process_hdutil.readAllStandardError();
  }

}

void MacDeployTool::FixupFramework(const QString &framework_name) {

  // Expected framework name looks like "Foo.framework"
  const QStringList parts = framework_name.split(u'.');
  if (parts.count() < 2) {
    LogError() << "FixupFramework: Unexpected framework name" << framework_name;
    return;
  }

  // Assume framework binary path is Foo.framework/Foo
  const QString framework_binary = framework_name + u'/' + parts[0];

  // Xcode expects to find Foo.framework/Versions/A when code signing, while qmake typically generates numeric versions.
  // Create symlink to the actual version in the framework.
  LinkFilePrintStatus(u"Current"_s, framework_name + "/Versions/A"_L1);

  // Set up @rpath structure.
  ChangeIdentification("@rpath/"_L1 + framework_binary, framework_binary);
  AddRPath(u"@loader_path/../../Contents/Frameworks/"_s, framework_binary);

}

bool MacDeployTool::Verify(const QString &appbundle_path) {

  LogNormal() << "Verifying" << appbundle_path;

  bool success = true;
  QDirIterator it(appbundle_path, QDir::Files | QDir::NoSymLinks, QDirIterator::Subdirectories);
  while (it.hasNext()) {
    it.next();

    const QString filepath = it.fileInfo().filePath();
    if (!IsMachOFile(filepath)) {
      continue;
    }

    QProcess process_otool;
    process_otool.start(u"otool"_s, QStringList() << u"-L"_s << filepath);
    process_otool.waitForFinished();
    if (process_otool.exitStatus() != QProcess::NormalExit || process_otool.exitCode() != 0) {
      LogError() << "otool failed for" << filepath << ":" << process_otool.readAllStandardError();
      success = false;
      continue;
    }
    const QString output = QString::fromLocal8Bit(process_otool.readAllStandardOutput());
    const QStringList output_lines = output.split(u"\n"_s, Qt::SkipEmptyParts);
    if (output_lines.size() < 2) {
      LogError() << "Could not parse otool output:" << output;
      success = false;
      continue;
    }
    QString first_line = output_lines.first();
    if (first_line.endsWith(u':')) {
      first_line.chop(1);
    }
    if (!first_line.startsWith(filepath)) {
      LogError() << "First line" << first_line << "does not match" << filepath;
      success = false;
    }
    static const QRegularExpression regexp(QStringLiteral("^\\t(.+) \\(compatibility version (\\d+\\.\\d+\\.\\d+), current version (\\d+\\.\\d+\\.\\d+)(, weak|, reexport)?\\)$"));
    for (const QString &output_line : output_lines) {

      if (first_line.startsWith(filepath)) {
        continue;
      }

      const QRegularExpressionMatch regexp_match = regexp.match(output_line);
      if (regexp_match.hasMatch()) {
        const QString library = regexp_match.captured(1);
        if (QFileInfo(library).fileName() == QFileInfo(filepath).fileName()) {  // It's this.
          continue;
        }
        else if (library.startsWith("@executable_path"_L1)) {
          QString real_path = library;
          real_path = real_path.replace("@executable_path"_L1, appbundle_path + "/Contents/MacOS"_L1);
          if (!QFile::exists(real_path)) {
            LogError() << real_path << "does not exist for" << filepath;
            success = false;
          }
        }
        else if (library.startsWith("@rpath"_L1)) {
          QString real_path = library;
          real_path = real_path.replace("@rpath"_L1, appbundle_path + "/Contents/Frameworks"_L1);
          if (!QFile::exists(real_path) && !real_path.endsWith("QtSvg"_L1)) {  // FIXME: Ignore broken svg image plugin.
            LogError() << real_path << "does not exist for" << filepath;
            success = false;
          }
        }
        else if (library.startsWith("@loader_path"_L1)) {
          const QString loader_path = QFileInfo(filepath).path();
          QString real_path = library;
          real_path = real_path.replace("@loader_path"_L1, loader_path);
          if (!QFile::exists(real_path)) {
            LogError() << real_path << "does not exist for" << filepath;
            success = false;
          }
        }
        else if (library.startsWith("/System/Library/"_L1) || library.startsWith("/usr/lib/"_L1)) {  // System library
          continue;
        }
        else {
          LogError() << "File" << filepath << "points to" << library;
          success = false;
        }
      }
      else {
        LogError() << "Could not parse otool output line:" << output_line;
        success = false;
      }
    }
  }

  return success;

}
