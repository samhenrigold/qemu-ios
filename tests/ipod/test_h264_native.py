#!/usr/bin/env python3
"""Compare hardware-derived I/P NALs and the real native DMA bridge.

macOS: python3 tests/ipod/test_h264_native.py movie.mov [--software]
The software check accepts multiple slices and verifies deferred DMA writes.
Uses ffmpeg's trace_headers as the independent source of parsed slice fields.
No guest assets are bundled. Requires access to native media services.
For --software, point PKG_CONFIG_PATH at the native package's patched FFmpeg.
"""
from pathlib import Path
import re
import argparse
import shlex
import subprocess
import tempfile

root = Path(__file__).resolve().parents[2]
parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('movie', type=Path)
parser.add_argument('--software', action='store_true', help='exercise incremental libavcodec slices')
args = parser.parse_args()
movie = args.movie.resolve()
trace = subprocess.run(['ffmpeg', '-v', 'trace', '-i', str(movie), '-map', '0:v',
    '-c', 'copy', '-bsf:v', 'trace_headers', '-f', 'null', '-'],
    capture_output=True, text=True, check=True).stderr
records = []
parameters = {}
fields = None
end = 0
packet_index = -1
for line in trace.splitlines():
    if 'Packet:' in line:
        if fields is not None:
            records.append((fields, end))
            fields = None
        packet_index += 1
    if 'Slice Header' in line:
        if fields is not None:
            records.append((fields, end))
        fields = parameters.copy()
        fields['_packet'] = packet_index
        fields['_modifications'] = []
    match = re.search(r'\]\s+(\d+)\s+([\w\[\]]+)\s+([01]+)\s+=\s+(-?\d+)', line)
    if not match:
        continue
    pos, name, bits, value = match.groups()
    parameters[name] = int(value)
    if fields is not None:
        fields[name] = int(value)
        if name in ('modification_of_pic_nums_idc', 'abs_diff_pic_num_minus1'):
            fields['_modifications'].append((name, int(value)))
        end = int(pos) + len(bits) - 8
if fields is not None:
    records.append((fields, end))
rows = []
dpb = []
previous = None
for f, bit in records:
    assert f['slice_type'] % 5 in (0, 2)
    assert args.software or f['first_mb_in_slice'] == 0
    assert f['frame_mbs_only_flag'] and f['nal_ref_idc']
    assert not f.get('long_term_reference_flag', 0)
    assert not f.get('adaptive_ref_pic_marking_mode_flag', 0)
    if previous is None or f['_packet'] != previous['_packet']:
        if previous is not None:
            dpb.append((previous['frame_num'], previous['_packet'] % 16))
            dpb = dpb[-f['max_num_ref_frames']:]
        if f['nal_unit_type'] == 5:
            dpb = []
    previous = f
    refs = 0
    ordered = []
    if f['slice_type'] % 5 == 0:
        refs = 1 + (f['num_ref_idx_l0_active_minus1'] if f['num_ref_idx_active_override_flag'] else f['num_ref_idx_l0_default_active_minus1'])
        modulus = 1 << (f['log2_max_frame_num_minus4'] + 4)
        current = f['frame_num']
        def picnum(picture):
            return picture[0] - (modulus if picture[0] > current else 0)
        ordered = sorted(dpb, key=picnum, reverse=True)
        if f['ref_pic_list_modification_flag_l0']:
            operations = iter(f['_modifications'])
            predicted = current
            index = 0
            for name, operation in operations:
                assert name == 'modification_of_pic_nums_idc'
                if operation == 3:
                    break
                assert operation in (0, 1), 'long-term references need a separate fixture'
                name, difference = next(operations)
                assert name == 'abs_diff_pic_num_minus1'
                predicted = (predicted + (difference + 1) * (-1 if operation == 0 else 1)) % modulus
                number = predicted - (modulus if predicted > current else 0)
                picture = next(p for p in dpb if picnum(p) == number)
                ordered = ordered[:index] + [picture] + [p for p in ordered[index:] if p != picture]
                index += 1
        assert 1 <= refs <= 16 and len(ordered) >= refs
    weighted = f['weighted_pred_flag']
    ld = f.get('luma_log2_weight_denom', 0) if weighted else 0
    cd = f.get('chroma_log2_weight_denom', 0) if weighted else 0
    row = [bit, 26 + f['pic_init_qp_minus26'] + f['slice_qp_delta'],
        f['chroma_qp_index_offset'], f['disable_deblocking_filter_idc'],
        2 * f.get('slice_alpha_c0_offset_div2', 0), 2 * f.get('slice_beta_offset_div2', 0), f['slice_type'] % 5, f['entropy_coding_mode_flag'],
        f.get('cabac_init_idc', 0) if f['slice_type'] % 5 == 0 else 0,
        weighted, ld, cd, f['first_mb_in_slice'], refs, f['constrained_intra_pred_flag']]
    for i, picture in enumerate(ordered[:refs]):
        lw, lo = ((f[f'luma_weight_l0[{i}]'], f[f'luma_offset_l0[{i}]'])
            if weighted and f.get(f'luma_weight_l0_flag[{i}]', 0) else (1 << ld, 0))
        chroma_weights = 0
        for ch in range(2):
            if weighted and f.get(f'chroma_weight_l0_flag[{i}]', 0):
                cw, co = f[f'chroma_weight_l0[{i}][{ch}]'], f[f'chroma_offset_l0[{i}][{ch}]']
            else:
                cw, co = 1 << cd, 0
            chroma_weights |= ((cw & 255) | ((co & 255) << 8)) << (16 * ch)
        row += [picture[1], (lw & 255) | ((lo & 255) << 8), chroma_weights]
    rows.append(row)
source = (root / 'hw/arm/ipod_touch_h264.c').read_text()
helpers = source[source.index('typedef struct IPodH264State'):source.index('static uint64_t h264_read')]
video = (root / 'hw/arm/ipod_video.c').read_text()
video = '\n'.join(x for x in video.splitlines() if not x.startswith('#include'))
prelude = r'''
#import <AVFoundation/AVFoundation.h>
#import <VideoToolbox/VideoToolbox.h>
#include <glib.h>
#include <assert.h>
#include <stdint.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
typedef int SysBusDevice;
typedef int MemoryRegion;
typedef int qemu_irq;
typedef uint64_t hwaddr;
typedef struct IPodVideoDecoder IPodVideoDecoder;
void ipod_video_close(IPodVideoDecoder *d);
#define warn_report(...) fprintf(stderr, __VA_ARGS__)
#define clz32(x) __builtin_clz(x)
#define MEMTXATTRS_UNSPECIFIED 0
static int address_space_memory;
static uint8_t output[2048*2048*3/2];
static uint8_t previous[16][sizeof(output)];
static unsigned reference_stride;
static const uint8_t *input;
static size_t input_size;
static void stl_be_p(void *p, uint32_t v) { v=GUINT32_TO_BE(v); memcpy(p,&v,4); }
static int address_space_read(void *as, hwaddr addr, int attrs, void *p, size_t n) {
    if(addr >= 0x0a000000 && addr < 0x10000000) {
        unsigned slot=(addr-0x0a000000)/reference_stride;
        unsigned offset=(addr-0x0a000000)%reference_stride;
        assert(slot<16 && n<=sizeof(previous[slot])-offset);
        memcpy(p,previous[slot]+offset,n);return 0;
    }
    assert(addr == 0x09f00000 && n <= input_size);memcpy(p,input,n);return 0;
}
static int address_space_write(void *as, hwaddr addr, int attrs, const void *p, size_t n) {
    assert(addr >= 0x08000000 && addr <= 0x08000000+sizeof(output));
    assert(n <= sizeof(output)-(addr-0x08000000));
    memcpy(output+(addr-0x08000000),p,n);return 0;
}
'''
body = r'''
int main(int argc, char **argv) { @autoreleasepool {
    assert(argc == 3);
    FILE *rows=fopen(argv[2],"r");assert(rows);
    AVURLAsset *asset=[AVURLAsset URLAssetWithURL:[NSURL fileURLWithPath:@(argv[1])] options:nil];
    AVAssetTrack *track=[[asset tracksWithMediaType:AVMediaTypeVideo] firstObject];assert(track);
    CMVideoFormatDescriptionRef format=(__bridge CMVideoFormatDescriptionRef)track.formatDescriptions.firstObject;
    CMVideoDimensions dim=CMVideoFormatDescriptionGetDimensions(format);
    assert(dim.width%16 == 0 && dim.height%16 == 0);
    unsigned plane=dim.width*dim.height, chroma_offset=(plane+1023)&~1023;
    reference_stride=(chroma_offset+plane/2+1023)&~1023;
    CFTypeRef range=CMFormatDescriptionGetExtension(format,kCMFormatDescriptionExtension_FullRangeVideo);
    bool full_range=range && CFEqual(range,kCFBooleanTrue);
    IPodVideoDecoder *reference=ipod_video_create(format,full_range ?
        kCVPixelFormatType_420YpCbCr8BiPlanarFullRange : kCVPixelFormatType_420YpCbCr8BiPlanarVideoRange);
    assert(reference);
    AVAssetReader *reader=[[AVAssetReader alloc] initWithAsset:asset error:nil];
    AVAssetReaderTrackOutput *stream=[AVAssetReaderTrackOutput assetReaderTrackOutputWithTrack:track outputSettings:nil];
    [reader addOutput:stream];assert([reader startReading]);
    unsigned frames=0;
    IPodH264State s={.rbsp=g_byte_array_new()};
    CMSampleBufferRef sample;
    while ((sample=[stream copyNextSampleBuffer])) {
        CMBlockBufferRef block=CMSampleBufferGetDataBuffer(sample);
        if(!block) { CFRelease(sample);continue; }
        size_t length=CMBlockBufferGetDataLength(block);
        g_autofree uint8_t *packet=g_malloc(length);
        assert(!CMBlockBufferCopyDataBytes(block,0,length,packet));
        assert(ipod_video_frame(reference,packet,length,0x08000000,0x08000000+chroma_offset));
        g_autofree uint8_t *expected=g_memdup2(output,sizeof(output));
        memset(s.regs,0,sizeof(s.regs));
        if(frames==90) h264_decoder_close(&s); /* fresh native session mid-stream */
        unsigned nals=0;
        memset(output,0xa5,sizeof(output));
        for(size_t off=0;off<length;) {
            assert(length-off>=4);
            uint32_t n;memcpy(&n,packet+off,4);n=GUINT32_FROM_BE(n);off+=4;
            assert(n && n<=length-off);
            if((packet[off]&31)==5 || (packet[off]&31)==1) {
                input=packet+off;input_size=n;nals++;
                s.regs[0x1200/4]=0x09f00000>>10;s.regs[0x1810/4]=n;
                h264_nal(&s);
                int qp,chroma,deblock,alpha,beta,type,cabac,init;
                unsigned weighted,ld,cd,first,active,constrained;
                assert(fscanf(rows,"%u %d %d %d %d %d %d %d %d %u %u %u %u %u %u",&s.bit,&qp,&chroma,&deblock,&alpha,&beta,&type,&cabac,&init,&weighted,&ld,&cd,&first,&active,&constrained)==15);
                s.regs[0x1030/4]=dim.width/16;s.regs[0x1034/4]=dim.height/16;
                s.regs[0x1028/4]=qp;s.regs[0x1024/4]=(chroma&31)|((chroma&31)<<5);
                s.regs[0x105c/4]=deblock;s.regs[0x1060/4]=alpha;s.regs[0x1064/4]=beta;
                s.regs[0x104c/4]=weighted;s.regs[0x1054/4]=ld;s.regs[0x1058/4]=cd;
                s.regs[0x1020/4]=cabac;s.regs[0x10cc/4]=init;
                s.regs[0x1018/4]=constrained;
                s.regs[0x102c/4]=type;s.regs[0x100c/4]=1;s.regs[0x10d4/4]=2;
                s.regs[0x120c/4]=0x08000000>>10;s.regs[0x125c/4]=(0x08000000+chroma_offset)>>10;
                s.regs[0x106c/4]=0x1703;
                s.regs[0x1040/4]=active ? active-1 : 0;
                for(unsigned i=0;i<active;i++) {
                    unsigned slot,lw,cw;
                    assert(fscanf(rows,"%u %u %u",&slot,&lw,&cw)==3 && slot<16);
                    s.regs[0x400/4+i]=lw;s.regs[0x480/4+i]=cw;
                    s.regs[0x100/4+i]=((64+i)<<8)|(4+i);
                    s.regs[0x1200/4+4+i]=(0x0a000000+slot*reference_stride)>>10;
                    s.regs[0x1200/4+64+i]=(0x0a000000+slot*reference_stride+chroma_offset)>>10;
                }
                s.regs[0x1038/4]=first/(dim.width/16);s.regs[0x103c/4]=first%(dim.width/16);
#ifdef IT_HAVE_AVCODEC
                bool accepted=h264_decode_software(&s);
                if(frames==0 && nals==1 && s.partial) {
                    /* A changed DMA destination cannot resume this picture. */
                    s.regs[0x103c/4]=1;s.regs[0x120c/4]++;
                    assert(!h264_decode_software(&s));
                    s.regs[0x103c/4]=0;s.regs[0x120c/4]--;
                    assert(h264_decode_software(&s)); /* restart from first slice */
                }
                if(s.partial) {
                    for(unsigned i=0;i<plane;i++) assert(output[i]==0xa5);
                    for(unsigned i=0;i<plane/2;i++) assert(output[chroma_offset+i]==0xa5);
                }
#else
                bool accepted=h264_decode(&s);
#endif
                if(!accepted) fprintf(stderr,"frame %u: type %d CABAC %d references %u\n",frames,type,cabac,active);
                assert(accepted);
            }
            off+=n;
        }
        assert(nals>=1);
        if(memcmp(output,expected,plane)) {
            fprintf(stderr,"frame %u: Y %u expected %u\n",frames,output[0],expected[0]);
        }
        assert(!memcmp(output,expected,plane));
        assert(!memcmp(output+chroma_offset,expected+chroma_offset,plane/2));
#ifdef IT_HAVE_AVCODEC
        if(frames==0) {
            s.regs[0x1038/4]=1;s.regs[0x103c/4]=0;
            assert(!h264_decode_software(&s)); /* orphan continuation, no active picture */
            assert(!memcmp(output,expected,plane));
        }
#endif
        memcpy(previous[frames%16],expected,sizeof(output));
        CFRelease(sample);frames++;
    }
    h264_decoder_close(&s);g_byte_array_unref(s.rbsp);
    ipod_video_close(reference);fclose(rows);
    printf("PASS: %u native H.264 I/P frames, all Y/UV pixels and DMA output match\n",frames);
}}
'''
with tempfile.TemporaryDirectory(prefix='h264-native-') as directory:
    main=Path(directory)/'check.m';exe=Path(directory)/'check';data=Path(directory)/'rows'
    data.write_text(''.join(' '.join(map(str,row))+'\n' for row in rows))
    extra = '#define IT_HAVE_AVCODEC 1\n#include <libavcodec/avcodec.h>\n#include <libavutil/opt.h>\n' if args.software else ''
    main.write_text('\n'.join((prelude,extra,video,helpers,body)))
    flags=shlex.split(subprocess.check_output(['pkg-config','--cflags','--libs','glib-2.0'] + (['libavcodec','libavutil'] if args.software else []),text=True))
    rpaths=[f'-Wl,-rpath,{flag[2:]}' for flag in flags if flag.startswith('-L')]
    subprocess.run(['clang','-Wno-deprecated-declarations','-fsanitize=address,undefined',
        '-fno-sanitize-recover=all',str(main),*flags,*rpaths,'-framework','AVFoundation','-framework',
        'VideoToolbox','-framework','CoreMedia','-framework','CoreVideo','-framework',
        'Foundation','-o',str(exe)],check=True)
    subprocess.run([str(exe),str(movie),str(data)],check=True)
