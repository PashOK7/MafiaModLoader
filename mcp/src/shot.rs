//! Screenshots: the raw frames the loader writes (`<game>\mml\shots\<id>.bgra`: 16 bytes {"MMLS", width, height, 0},
//! then BGRA rows, top first; host/screenshot.cpp) made into JPEGs next to them.

use std::path::{Path, PathBuf};

pub struct Shot {
    pub path: PathBuf,
    pub width: u32,
    pub height: u32,
    /// The game's frame as it was (before shrinking).
    pub frame: (u32, u32),
    pub jpeg: Vec<u8>,
}

/// How many JPEGs the folder keeps (the oldest go).
const KEEP: usize = 200;

/// A raw frame of the loader's: its width, height and BGRA pixels (rows top first).
pub struct Frame {
    pub width: usize,
    pub height: usize,
    pub bgra: Vec<u8>,
}

pub fn read_raw(raw: &Path) -> Result<Frame, String> {
    let mut data = std::fs::read(raw).map_err(|e| format!("{}: {e}", raw.display()))?;
    if data.len() < 16 || &data[0..4] != b"MMLS" {
        return Err(format!("{}: not a frame of the loader's", raw.display()));
    }
    let word = |i: usize| u32::from_le_bytes([data[i], data[i + 1], data[i + 2], data[i + 3]]) as usize;
    let (w, h) = (word(4), word(8));
    if w == 0 || h == 0 || w > 16384 || h > 16384 || data.len() < 16 + w * h * 4 {
        return Err(format!("{}: {w}x{h} does not fit its {} bytes", raw.display(), data.len()));
    }
    data.drain(..16);
    data.truncate(w * h * 4);
    Ok(Frame { width: w, height: h, bgra: data })
}

/// Two frames of one size compared, channel by channel (R, G, B): the mean and the worst absolute difference, the
/// pixels where any channel differs by more than `threshold`; the difference written to `image` as a JPEG (each
/// channel's difference times 8: a difference of 1 seen, 32 and more white).
pub fn compare(a: &Frame, b: &Frame, threshold: u8, image: &Path) -> Result<serde_json::Value, String> {
    if (a.width, a.height) != (b.width, b.height) {
        return Err(format!("the pictures differ in size: {}x{} and {}x{}", a.width, a.height, b.width, b.height));
    }
    let n = a.width * a.height;
    let mut sum = [0u64; 3];
    let mut worst = [0u8; 3];
    let mut over = 0usize;
    let mut rgb = Vec::with_capacity(n * 3);
    for (pa, pb) in a.bgra.chunks_exact(4).zip(b.bgra.chunks_exact(4)) {
        let mut any = false;
        for (c, i) in [2usize, 1, 0].into_iter().enumerate() {   // (R, G, B from BGRA)
            let d = pa[i].abs_diff(pb[i]);
            sum[c] += d as u64;
            worst[c] = worst[c].max(d);
            any |= d > threshold;
            rgb.push(d.saturating_mul(8));
        }
        over += any as usize;
    }
    let mean: Vec<f64> = sum.iter().map(|s| (*s as f64 / n as f64 * 1000.0).round() / 1000.0).collect();
    let mut jpeg = Vec::new();
    jpeg_encoder::Encoder::new(&mut jpeg, 90)
        .encode(&rgb, a.width as u16, a.height as u16, jpeg_encoder::ColorType::Rgb)
        .map_err(|e| format!("JPEG: {e}"))?;
    std::fs::write(image, &jpeg).map_err(|e| format!("{}: {e}", image.display()))?;
    Ok(serde_json::json!({"mean": mean, "worst": worst, "threshold": threshold, "over": over,
                          "over_percent": (over as f64 / n as f64 * 10000.0).round() / 100.0,
                          "image": image.display().to_string()}))
}

/// The raw frame at `raw` as `<id>.jpg` beside it, at most `max_width` wide (0: as it is): shrunk by a whole factor,
/// each pixel the mean of its block. The raw file is deleted.
pub fn convert(raw: &Path, max_width: u32, quality: u8) -> Result<Shot, String> {
    let f = read_raw(raw)?;
    convert_frame(&f, raw, max_width, quality)
}

/// A frame read (read_raw) as `<raw's id>.jpg`, as `convert`; the raw file deleted.
pub fn convert_frame(frame: &Frame, raw: &Path, max_width: u32, quality: u8) -> Result<Shot, String> {
    let (w, h) = (frame.width, frame.height);
    let px = &frame.bgra[..];
    let f = if max_width == 0 { 1 } else { w.div_ceil(max_width as usize).max(1) };
    let (ow, oh) = (w / f, h / f);
    let mut rgb = Vec::with_capacity(ow * oh * 3);
    let n = (f * f) as u32;
    for y in 0..oh {
        for x in 0..ow {
            let mut sum = [0u32; 3];
            for dy in 0..f {
                let row = ((y * f + dy) * w + x * f) * 4;
                for dx in 0..f {
                    let i = row + dx * 4;
                    sum[0] += px[i + 2] as u32;
                    sum[1] += px[i + 1] as u32;
                    sum[2] += px[i] as u32;
                }
            }
            rgb.extend(sum.iter().map(|s| ((s + n / 2) / n) as u8));
        }
    }
    let mut jpeg = Vec::new();
    jpeg_encoder::Encoder::new(&mut jpeg, quality.clamp(1, 100))
        .encode(&rgb, ow as u16, oh as u16, jpeg_encoder::ColorType::Rgb)
        .map_err(|e| format!("JPEG: {e}"))?;
    let path = raw.with_extension("jpg");
    std::fs::write(&path, &jpeg).map_err(|e| format!("{}: {e}", path.display()))?;
    let _ = std::fs::remove_file(raw);
    if let Some(dir) = path.parent() {
        prune(dir);
    }
    Ok(Shot { path, width: ow as u32, height: oh as u32, frame: (w as u32, h as u32), jpeg })
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn a_raw_frame_becomes_a_jpeg_half_as_wide() {
        let dir = std::env::temp_dir().join(format!("mml-shot-test-{}", std::process::id()));
        std::fs::create_dir_all(&dir).unwrap();
        let raw = dir.join("7.bgra");
        let (w, h) = (4u32, 2u32);
        let mut data = b"MMLS".to_vec();
        for v in [w, h, 0] { data.extend(v.to_le_bytes()); }
        for i in 0..w * h { data.extend([i as u8 * 30, 0, 255 - i as u8 * 30, 255]); }
        std::fs::write(&raw, &data).unwrap();
        let shot = convert(&raw, 2, 85).unwrap();
        assert_eq!((shot.width, shot.height, shot.frame), (2, 1, (4, 2)));
        assert_eq!(&shot.jpeg[..2], &[0xFF, 0xD8]);
        assert!(shot.path.ends_with("7.jpg") && shot.path.exists() && !raw.exists());
        std::fs::write(&raw, b"MMLS\x04\0\0\0\x02\0\0\0").unwrap();
        assert!(convert(&raw, 0, 85).is_err());
        std::fs::remove_dir_all(&dir).unwrap();
    }

    #[test]
    fn two_frames_compared_by_channel() {
        let dir = std::env::temp_dir().join(format!("mml-shot-cmp-{}", std::process::id()));
        std::fs::create_dir_all(&dir).unwrap();
        let a = Frame { width: 2, height: 2, bgra: vec![10, 20, 30, 255, 0, 0, 0, 255, 5, 5, 5, 255, 100, 100, 100, 255] };
        let mut b = a.bgra.clone();
        b[0] = 14;   // pixel 0: blue +4
        b[14] = 140; // pixel 3: red +40
        let b = Frame { width: 2, height: 2, bgra: b };
        let r = compare(&a, &b, 8, &dir.join("d.jpg")).unwrap();
        assert_eq!(r["worst"], serde_json::json!([40, 0, 4]));
        assert_eq!(r["mean"], serde_json::json!([10.0, 0.0, 1.0]));
        assert_eq!((r["over"].as_u64(), r["over_percent"].as_f64()), (Some(1), Some(25.0)));
        assert!(dir.join("d.jpg").exists());
        let c = Frame { width: 1, height: 4, bgra: a.bgra.clone() };
        assert!(compare(&a, &c, 8, &dir.join("e.jpg")).is_err());
        std::fs::remove_dir_all(&dir).unwrap();
    }
}

fn prune(dir: &Path) {
    let Ok(entries) = std::fs::read_dir(dir) else { return };
    let mut shots: Vec<(std::time::SystemTime, PathBuf)> = entries
        .flatten()
        .filter(|e| e.path().extension().is_some_and(|x| x == "jpg"))
        .filter_map(|e| Some((e.metadata().ok()?.modified().ok()?, e.path())))
        .collect();
    if shots.len() <= KEEP {
        return;
    }
    shots.sort();
    for (_, p) in &shots[..shots.len() - KEEP] {
        let _ = std::fs::remove_file(p);
    }
}
