# NDI2SRT - NDI to SRT Transcoder with SMPTE Timecode Injection

A high-performance transcoder that converts NDI (Network Device Interface) video streams to SRT (Secure Reliable Transport) with embedded SMPTE 12M-1 LTC timecode metadata. The application is designed for professional broadcast workflows where accurate timecode preservation is critical.

## Overview

NDI2SRT bridges the gap between NDI-based production environments and SRT streaming infrastructure, ensuring that timecode information from the source is properly embedded in the output H.264 stream for downstream processing by professional video tools.

## Prerequisites

### System Requirements

- **GStreamer**: 1.20+ with base/good/bad/ugly plugins
- **SRT Plugin**: gst-plugins-bad with SRT support
- **NDI SDK**: NewTek NDI SDK and GStreamer NDI plugin providing `ndisrc`
- **Platform Support**: Linux (Debian/Ubuntu) and macOS

### Installation by Platform

#### Linux (Debian/Ubuntu)
```bash
sudo apt-get update
sudo apt-get install -y cmake build-essential pkg-config \
  gstreamer1.0-tools gstreamer1.0-plugins-base gstreamer1.0-plugins-good \
  gstreamer1.0-plugins-bad gstreamer1.0-plugins-ugly gstreamer1.0-libav

# Install NDI SDK + GStreamer NDI plugin (varies by distro/vendor)
```

#### macOS
```bash
brew install cmake pkg-config gstreamer gst-plugins-base gst-plugins-good gst-plugins-bad gst-plugins-ugly gst-libav

# Install NDI SDK + GStreamer NDI plugin (ensure `ndisrc` is available)
# Optionally use VideoToolbox encoder: vtenc_h264
```

## Build

```bash
# Clone and build
git clone <repository-url>
cd ndi2srt
cmake -S . -B build
cmake --build build --config Release -j

# The binary will be available at: build/ndi2srt
```

## Usage

### Discovering NDI Sources

Before connecting to an NDI source, you can discover what's available on your network:

```bash
# List available NDI sources
./ndi2srt --discover
```

This will automatically scan your network for active NDI sources and display:
- **Source Names**: Exact names to use with `--ndi-name`
- **Network Information**: IP addresses and ports
- **Device Classes**: Source types (Audio/Video/Network)

Example output:
```
Available NDI sources:
=====================
  1. "Studio Camera 1" (class: Source/Audio/Video/Network)
      NDI Name: "Studio Camera 1"
      URL Address: 192.168.1.100:5962

  2. "OBS Virtual Camera" (class: Source/Audio/Video/Network)
      NDI Name: "OBS Virtual Camera"
      URL Address: 127.0.0.1:5962
```

### Basic SRT Streaming

```bash
# Caller mode (connect to remote SRT server)
./ndi2srt --ndi-name "My NDI Source" --srt-uri "srt://receiver:9000?mode=caller" \
  --encoder x264enc --bitrate 6000 --zerolatency

# Listener mode (wait for SRT client connection)
./ndi2srt --ndi-name "My NDI Source" --srt-uri "srt://:9000?mode=listener" --encoder x264enc
```

### Stdout Mode (FFmpeg Integration)

```bash
# Output MPEG-TS to stdout for further processing
./ndi2srt --ndi-name "My NDI Source" --stdout --timeout 10 | \
  ffmpeg -i - -c copy output.mp4

# Real-time streaming to FFmpeg
./ndi2srt --ndi-name "My NDI Source" --stdout | \
  ffmpeg -i - -f mpegts udp://127.0.0.1:1234
```

### Advanced Configurations

```bash
# macOS using VideoToolbox hardware encoder
./ndi2srt --ndi-name "My NDI Source" --srt-uri "srt://receiver:9000?mode=caller" \
  --encoder vtenc_h264 --bitrate 8000

# Custom timestamp mode for NDI source
./ndi2srt --ndi-name "My NDI Source" --srt-uri "srt://receiver:9000?mode=caller" \
  --timestamp-mode timecode --encoder x264enc

# Custom GOP size for broadcast workflows
./ndi2srt --ndi-name "My NDI Source" --stdout --gop-size 25 --bitrate 8000 --timeout 10 > broadcast.ts

# Debug mode with verbose output
./ndi2srt --ndi-name "My NDI Source" --stdout --verbose --timeout 5 > output.ts 2>debug.log
```

## Command-Line Options

The application provides a comprehensive set of command-line options organized into logical groups:

### **Required Options**
- `--ndi-name <name>` - NDI source name to connect to

### **Output Options**
- `--srt-uri <uri>` - SRT endpoint URI (srt://host:port?mode=caller)
- `--stdout` - Output MPEG-TS to stdout instead of SRT

### **Encoding Options**
- `--encoder <name>` - Video encoder: x264enc, vtenc_h264, openh264enc
- `--bitrate <kbps>` - Video bitrate in kbps (default: 6000)
- `--gop-size <frames>` - GOP size in frames (0 = auto, default: 0)

### **Behavior Options**
- `--no-audio` - Disable audio processing
- `--zerolatency` - Enable ultra-low latency mode (default: on)
- `--no-sei` - Disable SEI timecode injection
- `--timeout <seconds>` - Auto-exit after specified seconds (0 = disabled)
- `--dump-ts <path>` - Save MPEG-TS to file for debugging
- `--timestamp-mode <mode>` - NDI timestamp mode: auto, timecode, timestamp, etc.
- `--verbose` - Enable debug stderr messages
- `--discover` - Discover and list available NDI sources
- `--help`, `-h` - Show usage information

Run `./ndi2srt --help` for the complete, up-to-date help message.

## How It Works Internally

### Architecture and Pipeline

The application is built on GStreamer and constructs a real-time transcoding pipeline that processes NDI streams frame-by-frame. The core pipeline consists of:

1. **NDI Source (`ndisrc`)**: Captures the NDI stream with configurable timestamp modes
2. **NDI Demuxer (`ndisrcdemux`)**: Separates video and audio streams
3. **Video Processing**: Converts to I420 format and applies H.264 encoding
4. **Metadata Injection**: Injects SMPTE timecode via H.264 SEI (Supplemental Enhancement Information)
5. **Output**: Streams to SRT endpoint or outputs MPEG-TS to stdout

### Timecode Origin and Flow

In the default operation mode the timecode originates from the NDI source as UTC-based LTC (Linear Time Code) metadata embedded in the `GstVideoTimeCodeMeta`. This metadata contains:

- **Hours, Minutes, Seconds, Frames**: Standard SMPTE timecode components
- **Drop Frame Flag**: Indicates whether the source uses drop-frame timecode
- **Frame Rate**: Derived from the NDI stream's timing information

The timecode flows through the pipeline as follows:

1. **Source Extraction**: NDI source provides `GstVideoTimeCodeMeta` with each video frame
2. **PTS Fallback**: If timecode metadata is unavailable, the system falls back to deriving timecode from presentation timestamps (PTS)
3. **SEI Construction**: Timecode values are packed into H.264 Picture Timing SEI payloads
4. **Stream Injection**: SEI NAL units are inserted into the H.264 bitstream before each video frame

### Frame Metadata Injection Process

The application implements a sophisticated frame metadata injection system that ensures compliance with H.264 standards and FFmpeg compatibility:

#### SEI (Supplemental Enhancement Information) Injection

- **Picture Timing SEI (Payload Type 1)**: Standard H.264 SEI containing timecode information
- **SPS VUI Patching**: Modifies Sequence Parameter Set to signal `pic_struct_present_flag=1` and `timing_info_present_flag=1`
- **HRD Compliance**: Ensures compliance with ISO/IEC 14496-10-2005 D.1.2 requirements

#### Injection Strategy

The SEI injection occurs at the encoder output stage using a pad probe that:

1. **Analyzes Access Units**: Scans H.264 NAL units to identify frame boundaries
2. **SPS Management**: Caches and injects patched SPS with proper VUI flags
3. **SEI Placement**: Inserts Picture Timing SEI after AUD (Access Unit Delimiter) or at frame start
4. **Buffer Reconstruction**: Rebuilds complete H.264 Access Units with injected metadata

#### Timecode Format

The injected timecode follows SMPTE-12M standard:
- **Frame Count**: 0-23 (for 24fps) or 0-29 (for 30fps)
- **Seconds**: 0-59
- **Minutes**: 0-59  
- **Hours**: 0-23
- **Drop Frame**: Boolean flag for drop-frame timecode standards

### Transcoding Stream Settings

#### Video Encoding Configuration

- **Codec**: H.264 (AVC) using x264enc
- **Profile**: Baseline/High Profile with VUI parameters
- **Bitrate**: Configurable (default: 6000 kbps)
- **Latency**: Ultra-low latency mode with `tune=zerolatency`
- **Keyframe Interval**: Configurable GOP size (`--gop-size <frames>`, 0 = auto)
- **VUI Insertion**: Disabled (`insert-vui=false`) to allow manual SEI control

#### Stream Format

- **Container**: MPEG-TS (Transport Stream)
- **H.264 Format**: Annex B byte-stream with start codes
- **Alignment**: Access Unit (AU) aligned for proper parsing
- **SEI Structure**: Picture Timing SEI per frame with timecode data

#### Audio Processing

- **Codec Support**: AAC (default), MP3, AC3, SMPTE 302M
- **Bitrate Control**: Configurable via `--audio-bitrate <kbps>` (0 = auto)
- **Sample Rate**: Fixed at 48 kHz from NDI source
- **Channels**: Stereo (2 channels)
- **Format**: Varies by codec (S16LE for most, experimental for SMPTE 302M)
- **Disable Option**: Use `--no-audio` to exclude audio from output
- **Sync**: Maintained with video timing

### Output Modes

#### SRT Streaming Mode

- **Protocol**: SRT (Secure Reliable Transport)
- **Connection**: Caller or Listener mode
- **Latency**: Optimized for sub-100ms end-to-end latency
- **Reliability**: Built-in error correction and retransmission

#### Stdout Mode

- **Format**: Raw MPEG-TS stream to standard output
- **Use Case**: Piping to FFmpeg for further processing
- **Example**: `./ndi2srt --stdout | ffmpeg -i - -c copy output.mp4`

### Technical Implementation Details

#### GStreamer Integration

- **Pipeline Construction**: Uses `gst_parse_launch` for dynamic pipeline building
- **Pad Probes**: Custom probe functions for metadata injection
- **Buffer Management**: Direct buffer manipulation for SEI insertion
- **State Management**: Proper pipeline state transitions and cleanup

#### Memory Management

- **Buffer Pooling**: Efficient buffer allocation and recycling
- **SEI Caching**: Cached SPS and SEI structures to minimize recomputation
- **Zero-Copy**: Minimizes memory copies where possible

#### Error Handling

- **Stream Recovery**: Automatic recovery from NDI connection issues
- **SEI Validation**: Ensures injected metadata meets H.264 standards
- **Pipeline Monitoring**: Bus message handling for error detection

### Compatibility and Standards

#### FFmpeg Integration

The injected timecode is fully compatible with FFmpeg tools:

- **ffprobe**: Extracts timecode as `SMPTE 12-1 timecode` side data
- **ffmpeg**: Recognizes timecode metadata for filtering and processing
- **Side Data**: Accessible via `%{metadata:timecode}` in drawtext filters

#### H.264 Compliance

- **SEI Standards**: Follows ITU-T H.264 Annex D specifications
- **VUI Parameters**: Compliant with Video Usability Information standards
- **Bitstream Format**: Valid Annex B byte-stream with proper start codes

### Performance Characteristics

- **Latency**: Sub-100ms end-to-end for typical configurations
- **CPU Usage**: Optimized for real-time processing
- **Memory**: Efficient buffer management with minimal overhead
- **Scalability**: Supports multiple concurrent NDI sources

## Examples and Use Cases

### Broadcast Workflow Integration

```bash
# Discover available NDI sources first
./ndi2srt --discover

# Live production to SRT distribution
./ndi2srt --ndi-name "PC.LOCAL (Studio Camera 1)" --srt-uri "srt://cdn.example.com:9000?mode=caller" \
  --encoder x264enc --bitrate 8000 --verbose

# Multiple NDI sources to different SRT endpoints
./ndi2srt --ndi-name "PC.LOCAL (Camera 1)" --srt-uri "srt://endpoint1:9000?mode=caller" &
./ndi2srt --ndi-name "PC.LOCAL (Camera 2)" --srt-uri "srt://endpoint2:9000?mode=caller" &
```

### Content Creation and Streaming

```bash
# NDI to YouTube Live via FFmpeg
./ndi2srt --ndi-name "PC.LOCAL (OBS Virtual Camera)" --stdout | \
  ffmpeg -i - -c copy -f flv rtmp://a.rtmp.youtube.com/live2/STREAM_KEY

# Local recording with timecode
./ndi2srt --ndi-name "PC.LOCAL (Screen Capture)" --stdout --timeout 3600 | \
  ffmpeg -i - -c copy "recording_$(date +%Y%m%d_%H%M%S).mp4"
```

## Audio Codec Examples

### AAC Audio (Recommended)
```bash
# Default AAC with automatic bitrate
./ndi2srt --ndi-name "PC. LOCAL (Camera 1)" --stdout --audio-codec aac

# AAC with custom bitrate
./ndi2srt --ndi-name "PC.LOCAL (Camera 1)" --stdout --audio-codec aac --audio-bitrate 192
```

### MP3 Audio
```bash
# MP3 with 128 kbps bitrate
./ndi2srt --ndi-name "PC.LOCAL (Camera 1)" --stdout --audio-codec mp3 --audio-bitrate 128
```

### AC3 Audio (Dolby Digital)
```bash
# AC3 for broadcast applications
./ndi2srt --ndi-name "PC.LOCAL (Camera 1)" --stdout --audio-codec ac3 --audio-bitrate 256
```

### SMPTE 302M (Professional/Experimental)
```bash
# SMPTE 302M uncompressed audio (bitrate ignored)
# Note: Requires experimental codec support
./ndi2srt --ndi-name "PC.LOCAL (Camera 1)" --stdout --audio-codec smpte302m
```

### Disable Audio
```bash
# Video-only output
./ndi2srt --ndi-name "PC.LOCAL (Camera 1)" --stdout --no-audio
```
### Debugging and Testing

```bash
# Capture debug output and verify timecode injection
./ndi2srt --ndi-name "PC.LOCAL (Test Source)" --stdout --verbose --timeout 10 > test.ts 2>debug.log

# Verify timecode extraction
ffprobe -show_entries frame=side_data -of json test.ts | grep -A 5 "SMPTE 12-1 timecode"
```

## Troubleshooting

### Common Issues

1. **NDI Source Not Found**: Ensure NDI source is running and discoverable. Check NDI source name for spelling mistakes.
2. **SEI Injection Failing**: Check verbose output for SPS VUI parsing errors
3. **Timecode Not Extracting**: Verify ffprobe shows "SMPTE 12-1 timecode" side data
4. **High Latency**: Check network conditions and encoder settings

### Debug Mode

Use `--verbose` flag to enable detailed logging:
- SPS VUI parameter analysis
- SEI injection confirmation
- Timecode value logging
- Buffer processing details

### Performance Tuning

- **Bitrate**: Adjust based on network capacity and quality requirements
- **Encoder**: Use hardware encoders (vtenc_h264) when available
- **Latency**: Balance between `--zerolatency` and compression efficiency

This architecture ensures that professional broadcast workflows can maintain accurate timecode synchronization when transitioning from NDI production environments to SRT distribution networks, while providing the flexibility to output to various downstream systems.
