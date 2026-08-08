// This file is part of macdeploytool, a fork of macdeployqt
//
// Copyright (C) 2016 The Qt Company Ltd.
// Copyright (C) 2026 Jonas Kvinge <jonas@jkvinge.net>
//
// SPDX-License-Identifier: LicenseRef-Qt-Commercial OR GPL-3.0-only WITH Qt-GPL-exception-1.0

#ifndef MACDEPLOYTOOL_H
#define MACDEPLOYTOOL_H

#include <QSet>
#include <QList>
#include <QString>
#include <QStringList>
#include <QRegularExpression>

#include "frameworkinfo.h"
#include "dylibinfo.h"
#include "otoolinfo.h"
#include "applicationbundleinfo.h"
#include "deploymentinfo.h"
#include "gstreamerpluginset.h"

class MacDeployTool {

 public:
  void SetRunStripEnabled(const bool enabled) { run_strip_enabled_ = enabled; }
  void SetAlwaysOwerwriteEnabled(const bool enabled) { always_owerwrite_enabled_ = enabled; }
  void SetRunCodesign(const bool enabled) { run_codesign_ = enabled; }
  void SetHardenedRuntime(const bool enabled) { hardened_runtime_ = enabled; }
  void SetSecureTimestamp(const bool enabled) { secure_timestamp_ = enabled; }
  void SetAppstoreCompliant(const bool enabled) { appstore_compliant_ = enabled; }
  void SetLibrarySearchPath(const QStringList &library_search_path) { library_search_path_ = library_search_path; }

  DeploymentInfo DeployQtFrameworks(const QString &app_bundle_path, const QStringList &additional_executables, const bool use_debug_libs);
  void DeployPlugins(const QString &app_bundle_path, DeploymentInfo deployment_info, const bool use_debug_libs);
  void DeployGioModules(const QString &app_bundle_path, const DeploymentInfo &deployment_info, const bool use_debug_libs);
  bool DeployGStreamerPlugins(const QString &app_bundle_path, const DeploymentInfo &deployment_info, GStreamerPluginSet plugin_set, const QStringList &plugin_names, const bool use_debug_libs);
  void StripAppBinary(const QString &bundle_path);
  void Codesign(const QString &identity, const QString &app_bundle_path);
  void CreateDiskImage(const QString &app_bundle_path, const QString &filesystem_type);
  static void FixupFramework(const QString &app_bundle_path);
  static bool Verify(const QString &appbundle_path);
  static bool CheckRequiredTools(const bool run_strip, const bool run_codesign, const bool dmg);
  static bool CheckRelocatableQt();

 private:
  static bool IsMachOFile(const QString &path);
  bool CopyFilePrintStatus(const QString &source, const QString &destination);
  static bool LinkFilePrintStatus(const QString &file, const QString &link);
  static void Patch_debugInInfoPlist(const QString &info_plist_path);
  static OtoolInfo FindDependencyInfo(const QString &binary_path);
  FrameworkInfo ParseOtoolLibraryLine(const QString &line, const QString &app_bundle_path, const QList<QString> &rpaths, const bool use_debug_libs);
  static QString FindAppBinary(const QString &app_bundle_path);
  static QStringList FindAppFrameworkNames(const QString &app_bundle_path);
  static QStringList FindAppFrameworkPaths(const QString &app_bundle_path);
  static QStringList FindAppLibraries(const QString &app_bundle_path);
  static QStringList FindAppBundleFiles(const QString &app_bundle_path, const bool absolute_path = false);
  static QString FindEntitlementsFile(const QString &path);
  QList<FrameworkInfo> GetQtFrameworks(const QList<DylibInfo> &dependencies, const QString &app_bundle_path, const QList<QString> &rpaths, const bool use_debug_libs);
  static QString ResolveDyldPrefix(const QString &path, const QString &loader_path, const QString &executable_path);
  static QList<QString> GetBinaryRPaths(const QString &path, const bool resolve = true, QString executable_path = QString());
  QList<FrameworkInfo> GetQtFrameworks(const QString &path, const QString &app_bundle_path, const QList<QString> &rpaths, const bool use_debug_libs);
  QList<FrameworkInfo> GetQtFrameworksForPaths(const QStringList &paths, const QString &app_bundle_path, const QList<QString> &rpaths, const bool use_debug_libs);
  static QStringList GetBinaryDependencies(const QString executable_path, const QString &path, const QList<QString> &additional_binaries_containing_rpaths, const QString &app_bundle_path);
  bool RecursiveCopy(const QString &source_path, const QString &destination_path, const QRegularExpression &ignore_reg_exp = QRegularExpression());
  void RecursiveCopyAndDeploy(const QString &app_bundle_path, const QList<QString> &rpaths, const QString &source_path, const QString &destination_path);
  QString CopyDylib(const FrameworkInfo &framework, const QString path);
  QString CopyFramework(const FrameworkInfo &framework, const QString path);
  static void RunInstallNameTool(const QStringList &options);
  static void ChangeIdentification(const QString &id, const QString &binary_path);
  static void ChangeInstallName(const QString &bundle_path, const FrameworkInfo &framework, const QStringList &binary_paths, const bool use_loader_path);
  static void ChangeInstallName(const QString &old_name, const QString &new_name, const QString &binary_path);
  static void AddRPath(const QString &rpath, const QString &binary_path);
  static void DeployRPaths(const QString &bundle_path, const QList<QString> &rpaths, const QString &binary_path, const bool use_loader_path);
  static void DeployRPaths(const QString &bundle_path, const QList<QString> &rpaths, const QStringList &binary_paths, bool use_loader_path);
  void RunStrip(const QString &binary_path);
  DeploymentInfo DeployQtFrameworks(QList<FrameworkInfo> frameworks, const QString &bundle_path, const QStringList &binary_paths, const bool use_debug_libs, const bool use_loader_path);
  static QString GetLibInfix(const QStringList &deployed_frameworks);
  void DeployPlugins(const ApplicationBundleInfo &app_bundle_info, const QString &plugin_source_path, const QString plugin_destination_path, DeploymentInfo deployment_info, const bool use_debug_libs);
  static QString PkgConfigVariable(const QString &package, const QString &variable);
  static QString FindGioModulesDir(const QString &gio_modules_directory);
  static QString FindGStreamerPluginsDir(const QString &gstreamer_plugins_directory);
  static QString FindGstPluginScanner(const QString &plugins_dir);
  static QString ResolvePluginFile(const QString &plugin_name);
  static QStringList SelectGStreamerPluginFiles(const QString &plugins_dir, GStreamerPluginSet plugin_set, const QStringList &plugin_names);
  void DeployBinaryAndItsDependencies(const QString &app_bundle_path, const DeploymentInfo &deployment_info, bool use_debug_libs, const QString &source_path, const QString &destination_dir);
  static QString FindLibsoupLibrary();
  void CodesignFile(const QString &identity, const QString &file_path);
  QSet<QString> CodesignBundle(const QString &identity, const QString &app_bundle_path, QList<QString> additional_binaries_containing_rpaths);

  bool run_strip_enabled_ = true;
  bool always_owerwrite_enabled_ = false;
  bool run_codesign_ = true;
  QStringList library_search_path_;
  QString extra_entitlements_;
  bool hardened_runtime_ = false;
  bool secure_timestamp_ = false;
  bool appstore_compliant_ = false;

};

#endif  // MACDEPLOYTOOL_H
