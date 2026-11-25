#!/bin/bash
# Makes "T-MU Plugins.pkg": puts VST3, AU and AAX into the system plug-in folders.
set -euo pipefail
BUILD="$1"; NUM="${2:-0}"
STAGE="$(mktemp -d)"; mkdir -p dist
mkdir -p "$STAGE/VST3" "$STAGE/Components" "$STAGE/AAX"
for dir in "$BUILD"/*_artefacts/Release; do
  [ -d "$dir/VST3" ] && cp -R "$dir"/VST3/*.vst3 "$STAGE/VST3/"
  [ -d "$dir/AU" ] && cp -R "$dir"/AU/*.component "$STAGE/Components/"
  [ -d "$dir/AAX" ] && cp -R "$dir"/AAX/*.aaxplugin "$STAGE/AAX/"
done
# ad-hoc signature so macOS will load them (not notarised: see README)
for b in "$STAGE"/*/*; do codesign --force --deep --sign - "$b"; done
pkgbuild --root "$STAGE/VST3" --install-location "/Library/Audio/Plug-Ins/VST3" --identifier com.tmu.plugins.vst3 --version "1.0.$NUM" "$STAGE/vst3.pkg"
pkgbuild --root "$STAGE/Components" --install-location "/Library/Audio/Plug-Ins/Components" --identifier com.tmu.plugins.au --version "1.0.$NUM" "$STAGE/au.pkg"
pkgbuild --root "$STAGE/AAX" --install-location "/Library/Application Support/Avid/Audio/Plug-Ins" --identifier com.tmu.plugins.aax --version "1.0.$NUM" "$STAGE/aax.pkg"
cat > "$STAGE/dist.xml" <<XML
<?xml version="1.0" encoding="utf-8"?>
<installer-gui-script minSpecVersion="2">
  <title>T-MU Plugins</title>
  <options customize="allow" require-scripts="false" hostArchitectures="x86_64,arm64"/>
  <choices-outline>
    <line choice="vst3"/><line choice="au"/><line choice="aax"/>
  </choices-outline>
  <choice id="vst3" title="VST3 (Ableton, FL Studio, Studio One, Reaper...)"><pkg-ref id="com.tmu.plugins.vst3"/></choice>
  <choice id="au" title="Audio Unit (Logic, GarageBand)"><pkg-ref id="com.tmu.plugins.au"/></choice>
  <choice id="aax" title="AAX (Pro Tools)"><pkg-ref id="com.tmu.plugins.aax"/></choice>
  <pkg-ref id="com.tmu.plugins.vst3">vst3.pkg</pkg-ref>
  <pkg-ref id="com.tmu.plugins.au">au.pkg</pkg-ref>
  <pkg-ref id="com.tmu.plugins.aax">aax.pkg</pkg-ref>
</installer-gui-script>
XML
productbuild --distribution "$STAGE/dist.xml" --package-path "$STAGE" "dist/T-MU-Plugins-Mac.pkg"
( cd "$STAGE" && zip -qry "$OLDPWD/dist/T-MU-Plugins-Mac-files.zip" VST3 Components AAX )
ls -la dist
