// This file is part of macdeploytool, a fork of macdeployqt
//
// Copyright (C) 2016 The Qt Company Ltd.
// Copyright (C) 2026 Jonas Kvinge <jonas@jkvinge.net>
//
// SPDX-License-Identifier: LicenseRef-Qt-Commercial OR GPL-3.0-only WITH Qt-GPL-exception-1.0

#include <QCoreApplication>
#include <QCommandLineParser>
#include <QCommandLineOption>
#include <QString>
#include <QStringList>
#include <QDir>
#include <QLibraryInfo>

#include "logging.h"
#include "deploymentinfo.h"
#include "gstreamerpluginset.h"
#include "macdeploytool.h"

using namespace Qt::StringLiterals;

int main(int argc, char **argv) {

  QCoreApplication app(argc, argv);

  QCommandLineParser parser;
  parser.setApplicationDescription(
    u"macdeploytool takes an application bundle as input and makes it self-contained by copying in the Qt frameworks and plugins that the application uses.\n\n"
    "Plugins related to a framework are copied in with the framework. The accessibility, image formats, and text codec plugins are always copied, unless \"--no-plugins\" is specified.\n\n"
    "Qt plugins may use private API and will cause the app to be rejected from the Mac App Store. macdeploytool will print a warning when known incompatible plugins are deployed. Use --appstore-compliant to skip these plugins. Currently two SQL plugins are known to be incompatible: qsqlodbc and qsqlpsql.\n\n"
    "If the app depends on libgio-2.0, its GIO modules directory (gsettings backends, TLS backend, proxy resolvers, volume monitors) is bundled automatically."_s);

  parser.addPositionalArgument(u"app-bundle"_s, u"The application bundle to deploy."_s);

  const QCommandLineOption verbose_option(u"verbose"_s, u"0 = no output, 1 = error/warning (default), 2 = normal, 3 = debug."_s, u"level"_s, u"1"_s);
  parser.addOption(verbose_option);

  const QCommandLineOption no_plugins_option(u"no-plugins"_s, u"Skip plugin deployment."_s);
  parser.addOption(no_plugins_option);

  const QCommandLineOption dmg_option(u"dmg"_s, u"Create a .dmg disk image."_s);
  parser.addOption(dmg_option);

  const QCommandLineOption no_strip_option(u"no-strip"_s, u"Don't run 'strip' on the binaries."_s);
  parser.addOption(no_strip_option);

  const QCommandLineOption use_debug_libs_option(u"use-debug-libs"_s, u"Deploy with debug versions of frameworks and plugins (implies --no-strip)."_s);
  parser.addOption(use_debug_libs_option);

  const QCommandLineOption executable_option(u"executable"_s, u"Let the given executable use the deployed frameworks too."_s, u"path"_s);
  parser.addOption(executable_option);

  const QCommandLineOption always_overwrite_option(u"always-overwrite"_s, u"Copy files even if the target file exists."_s);
  parser.addOption(always_overwrite_option);

  const QCommandLineOption codesign_option(u"codesign"_s, u"Run codesign with the given identity on all executables (default: ad-hoc sign)."_s, u"identity"_s, u"-"_s);
  parser.addOption(codesign_option);

  const QCommandLineOption no_codesign_option(u"no-codesign"_s, u"Disable code signing."_s);
  parser.addOption(no_codesign_option);

  const QCommandLineOption hardened_runtime_option(u"hardened-runtime"_s, u"Enable Hardened Runtime when code signing."_s);
  parser.addOption(hardened_runtime_option);

  const QCommandLineOption timestamp_option(u"timestamp"_s, u"Include a secure timestamp when code signing (requires internet connection)."_s);
  parser.addOption(timestamp_option);

  const QCommandLineOption sign_for_notarization_option(u"sign-for-notarization"_s, u"Activate the necessary options for notarization (requires internet connection)."_s, u"identity"_s);
  parser.addOption(sign_for_notarization_option);

  const QCommandLineOption appstore_compliant_option(u"appstore-compliant"_s, u"Skip deployment of components that use private API."_s);
  parser.addOption(appstore_compliant_option);

  const QCommandLineOption libpath_option(u"libpath"_s, u"Add the given path to the library search path."_s, u"path"_s);
  parser.addOption(libpath_option);

  const QCommandLineOption fs_option(u"fs"_s, u"Set the filesystem used for the .dmg disk image."_s, u"filesystem"_s, u"HFS+"_s);
  parser.addOption(fs_option);

  const QCommandLineOption gstreamer_plugins_option(u"gstreamer-plugins"_s, u"Which GStreamer plugins to bundle if the app depends on libgstreamer-1.0: none (default), all, or list."_s, u"set"_s, u"none"_s);
  parser.addOption(gstreamer_plugins_option);

  const QCommandLineOption gstreamer_plugin_list_option(u"gstreamer-plugin-list"_s, u"Comma-separated GStreamer plugin names to bundle exactly (e.g. coreelements,playback,typefindfunctions,audioconvert); only used when --gstreamer-plugins=list."_s, u"names"_s);
  parser.addOption(gstreamer_plugin_list_option);

  QCommandLineOption deploy_framework_option(u"deploy-framework"_s, u"Undocumented: fix up an internal Qt framework bundle instead of an application bundle."_s);
  deploy_framework_option.setFlags(QCommandLineOption::HiddenFromHelp);
  parser.addOption(deploy_framework_option);

  parser.addHelpOption();

  parser.process(app);

  const QStringList positional_arguments = parser.positionalArguments();
  if (positional_arguments.isEmpty()) {
    parser.showHelp(1);
  }

  const QString appbundle_path = QDir::cleanPath(positional_arguments.first());
  if (!QDir(appbundle_path).exists()) {
    qDebug() << "Error: Could not find app bundle" << appbundle_path;
    return 1;
  }

  if (!MacDeployTool::CheckRelocatableQt()) {
    return 1;
  }

  MacDeployTool mac_deploy_tool;

  bool strip_enabled = true;
  bool codesign_enabled = false;
  bool hardened_runtime_enabled = false;
  bool secure_timestamp_enabled = false;
  bool appstore_compliant_enabled = false;
  bool deploy_framework_enabled = false;

  bool ok = false;
  const int verbose_level = parser.value(verbose_option).toInt(&ok);
  if (!ok) {
    LogError() << "Could not parse verbose level";
    return 1;
  }
  log_level = verbose_level;

  const QStringList option_names = parser.optionNames();
  for (const QString &option_name : option_names) {
    LogDebug() << "Argument found:" << option_name;
  }

  const bool plugins = !parser.isSet(no_plugins_option);
  const bool create_dmg_enabled = parser.isSet(dmg_option);
  bool use_debug_libs = parser.isSet(use_debug_libs_option);

  if (parser.isSet(no_strip_option) || use_debug_libs) {
    strip_enabled = false;
  }
  mac_deploy_tool.SetRunStripEnabled(strip_enabled);

  const QStringList additional_executables = parser.values(executable_option);

  if (parser.isSet(always_overwrite_option)) {
    mac_deploy_tool.SetAlwaysOwerwriteEnabled(true);
  }

  if (parser.isSet(no_codesign_option) && parser.isSet(codesign_option)) {
    LogError() << "Error: --no-codesign cannot be combined with --codesign";
    return 1;
  }

  if (parser.isSet(no_codesign_option) && parser.isSet(sign_for_notarization_option)) {
    LogError() << "Error: --no-codesign cannot be combined with --sign-for-notarization";
    return 1;
  }

  QString codesign_identity = parser.value(codesign_option);

  if (parser.isSet(codesign_option)) {
    codesign_enabled = true;
  }

  if (parser.isSet(sign_for_notarization_option)) {
    codesign_enabled = true;
    hardened_runtime_enabled = true;
    secure_timestamp_enabled = true;
    codesign_identity = parser.value(sign_for_notarization_option);
  }

  if (parser.isSet(hardened_runtime_option)) {
    hardened_runtime_enabled = true;
  }

  if (parser.isSet(timestamp_option)) {
    secure_timestamp_enabled = true;
  }

  if (parser.isSet(appstore_compliant_option)) {
    appstore_compliant_enabled = true;
  }

  if (parser.isSet(deploy_framework_option)) {
    deploy_framework_enabled = true;
  }

  if (!MacDeployTool::CheckRequiredTools(strip_enabled, codesign_enabled, create_dmg_enabled)) {
    return 1;
  }

  mac_deploy_tool.SetRunCodesign(codesign_enabled);
  mac_deploy_tool.SetHardenedRuntime(hardened_runtime_enabled);
  mac_deploy_tool.SetSecureTimestamp(secure_timestamp_enabled);
  mac_deploy_tool.SetAppstoreCompliant(appstore_compliant_enabled);
  mac_deploy_tool.SetLibrarySearchPath(parser.values(libpath_option));

  const QString filesystem_type = parser.value(fs_option);

  const QString gstreamer_plugins_value = parser.value(gstreamer_plugins_option).trimmed().toLower();

  QStringList gstreamer_plugin_names;
  if (parser.isSet(gstreamer_plugin_list_option)) {
    const QStringList raw_names = parser.value(gstreamer_plugin_list_option).split(','_L1);
    for (const QString &raw_name : raw_names) {
      const QString name = raw_name.trimmed();
      if (!name.isEmpty()) {
        gstreamer_plugin_names << name;
      }
    }
  }

  GStreamerPluginSet gstreamer_plugin_set;
  if (gstreamer_plugins_value == "none"_L1) {
    gstreamer_plugin_set = GStreamerPluginSet::None;
  }
  else if (gstreamer_plugins_value == "all"_L1) {
    gstreamer_plugin_set = GStreamerPluginSet::All;
  }
  else if (gstreamer_plugins_value == "list"_L1) {
    gstreamer_plugin_set = GStreamerPluginSet::List;
  }
  else {
    LogError() << "Invalid value for --gstreamer-plugins:" << gstreamer_plugins_value << "(expected none, all, or list)";
    return 1;
  }

  DeploymentInfo deployment_info = mac_deploy_tool.DeployQtFrameworks(appbundle_path, additional_executables, use_debug_libs);

  if (deployment_info.is_debug) {
    use_debug_libs = true;
  }

  if (deploy_framework_enabled && deployment_info.is_framework) {
    mac_deploy_tool.FixupFramework(appbundle_path);
  }

  if (plugins) {
    const QStringList plugin_paths = QLibraryInfo::paths(QLibraryInfo::PluginsPath);
    if (plugin_paths.isEmpty()) {
      LogError() << "Missing Qt plugins path\n";
      return 1;
    }

    deployment_info.plugin_path = plugin_paths.constFirst();
    if (!QDir(deployment_info.plugin_path).exists()) {
      LogError() << "Plugins path does not exist" << deployment_info.plugin_path << "\n";
      return 1;
    }

    Q_ASSERT(!deployment_info.plugin_path.isEmpty());
    if (!deployment_info.plugin_path.isEmpty()) {
      LogNormal();
      mac_deploy_tool.DeployPlugins(appbundle_path, deployment_info, use_debug_libs);
    }
  }

  mac_deploy_tool.DeployGioModules(appbundle_path, deployment_info, use_debug_libs);

  if (gstreamer_plugin_set != GStreamerPluginSet::None) {
    if (!mac_deploy_tool.DeployGStreamerPlugins(appbundle_path, deployment_info, gstreamer_plugin_set, gstreamer_plugin_names, use_debug_libs)) {
      return 1;
    }
  }

  if (strip_enabled) {
    mac_deploy_tool.StripAppBinary(appbundle_path);
  }

  if (codesign_enabled) {
    mac_deploy_tool.Codesign(codesign_identity, appbundle_path);
  }

  if (create_dmg_enabled) {
    LogNormal();
    mac_deploy_tool.CreateDiskImage(appbundle_path, filesystem_type);
  }

  if (log_errors > 0) {
    return 1;
  }

  if (!MacDeployTool::Verify(appbundle_path)) {
    return 1;
  }

  return 0;

}
