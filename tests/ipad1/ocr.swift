// ocr PPM: the text on a screendump, one line per piece of text: "x0 y0 x1 y1 text", in upright portrait pixels
// (origin top left). An ipad1 panel (1024x768, the portrait UI turned on it) is turned upright first; a portrait
// panel (n18, n88: no turn) is read as it is, so its pixels are the panel's own.
// Vision's text recognizer; tests/ipad1/regress.py builds this once (swiftc -O) and reads Setup pages with it.
import CoreGraphics
import Foundation
import Vision

let data = try! Data(contentsOf: URL(fileURLWithPath: CommandLine.arguments[1]))
// P6 header: magic, width, height, maxval, one whitespace byte, then RGB
var fields: [Int] = [], i = 2, cur = -1
while fields.count < 3 {
    let c = data[i]
    if c >= 48 && c <= 57 { cur = (cur < 0 ? 0 : cur) * 10 + Int(c - 48) } else if cur >= 0 { fields.append(cur); cur = -1 }
    i += 1
}
let (w, h) = (fields[0], fields[1])
let rgb = data.subdata(in: i..<(i + w * h * 3))
let panel = CGImage(width: w, height: h, bitsPerComponent: 8, bitsPerPixel: 24, bytesPerRow: w * 3,
                    space: CGColorSpaceCreateDeviceRGB(), bitmapInfo: CGBitmapInfo(rawValue: 0),
                    provider: CGDataProvider(data: rgb as CFData)!, decode: nil, shouldInterpolate: false,
                    intent: .defaultIntent)!
// Upright portrait: its top is the panel's left edge, its left the panel's bottom (a quarter turn).
let turned = w > h
// A 320x480 panel is read at twice its size: Vision misses or fails on text that small (whole pages empty).
let k = max(w, h) < 800 ? 2 : 1
let ctx = CGContext(data: nil, width: (turned ? h : w) * k, height: (turned ? w : h) * k, bitsPerComponent: 8,
                    bytesPerRow: 0, space: CGColorSpaceCreateDeviceRGB(), bitmapInfo: CGImageAlphaInfo.noneSkipLast.rawValue)!
ctx.interpolationQuality = .high
ctx.scaleBy(x: CGFloat(k), y: CGFloat(k))
if turned {
    ctx.translateBy(x: 0, y: CGFloat(w))
    ctx.rotate(by: -.pi / 2)
}
ctx.draw(panel, in: CGRect(x: 0, y: 0, width: w, height: h))
let portrait = ctx.makeImage()!

let req = VNRecognizeTextRequest()
req.recognitionLevel = .accurate
req.usesLanguageCorrection = false
// The accurate recognizer's model fails to load in bursts (CRImageReaderError e5rtError, under load): try it a
// few times, then read with the fast one (a different model, weaker on short words) rather than not at all.
var tries = 0
while true {
    do {
        try VNImageRequestHandler(cgImage: portrait, options: [:]).perform([req])
        break
    } catch {
        tries += 1
        FileHandle.standardError.write(Data("ocr: \(req.recognitionLevel == .accurate ? "accurate" : "fast") failed (\(error))\n".utf8))
        if tries == 4 { req.recognitionLevel = .fast } else if tries > 4 { exit(1) }
        Thread.sleep(forTimeInterval: 3)
    }
}
let (pw, ph) = turned ? (Double(h), Double(w)) : (Double(w), Double(h))
for o in req.results ?? [] {
    guard let t = o.topCandidates(1).first else { continue }
    let b = o.boundingBox          // normalized, origin bottom left
    print(String(format: "%d %d %d %d %@", Int(b.minX * pw), Int((1 - b.maxY) * ph), Int(b.maxX * pw),
                 Int((1 - b.minY) * ph), t.string))
}
