#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "$0")/.." && pwd)"

ROOT_DIR="$ROOT_DIR" ruby <<'RUBY'
require 'json'
require 'pathname'

root = ENV.fetch('ROOT_DIR')
presets_dir = File.join(root, 'src/presets')
dsp_path = File.join(root, 'src/dsp/chord_flow_plugin.c')

default_path = File.join(presets_dir, 'default.json')
if File.exist?(default_path)
  abort("FAIL: shipped default.json should not exist (FMC-only release)")
end

fmc_paths = Dir.glob(File.join(presets_dir, 'fmc_*.json')).sort
abort('FAIL: no shipped FMC preset files found') if fmc_paths.empty?
dsp = File.read(dsp_path)

type_block = dsp[/static const char \*TYPE_NAMES\[\] = \{(.*?)\};/m, 1]
abort('FAIL: could not parse TYPE_NAMES from chord_flow_plugin.c') unless type_block
allowed_types = type_block.scan(/"([^"]+)"/).flatten.to_h { |t| [t, true] }

errors = []

fmc_paths.each do |path|
  presets = JSON.parse(File.read(path))
  file_label = Pathname.new(path).basename.to_s

  presets.each do |preset|
    name = preset['name'] || '(unnamed)'
    pads = preset['pads'] || []

    if pads.length != 16
      errors << "#{file_label}/#{name}: expected 16 pads, got #{pads.length}"
    end

    seen = {}
    pads.each_with_index do |pad, i|
      type = pad['chord_type'].to_s
      unless allowed_types[type]
        errors << "#{file_label}/#{name} pad #{i + 1}: unsupported chord_type '#{type}'"
      end

      sig = [pad['root'], type, pad['inversion'], pad['bass'], pad['octave']].join('|')
      if seen.key?(sig)
        errors << "#{file_label}/#{name}: exact duplicate pad recipe at #{seen[sig]} and #{i + 1} (#{sig})"
      else
        seen[sig] = i + 1
      end
    end
  end
end

if errors.any?
  puts "FAIL: shipped FMC preset quality checks failed"
  errors.each { |e| puts " - #{e}" }
  exit 1
end

puts "PASS: shipped FMC preset quality"
RUBY
