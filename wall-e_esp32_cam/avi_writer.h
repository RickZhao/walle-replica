/**
 * MINIMAL MJPEG AVI WRITER (Arduino FS)
 *
 * @file    avi_writer.h
 * @brief   Writes JPEG frames into a standard AVI (RIFF) container so
 *          recordings play directly on PC/phone. ESP32-S3 has no video
 *          encoder, so MJPEG-in-AVI is the only realistic format.
 *
 * Usage:
 *   File f = SD.open("/videos/VID_0001.avi", FILE_WRITE);
 *   AviWriter avi;
 *   avi.Begin(f, 640, 480, 15);
 *   avi.AddFrame(fb->buf, fb->len);   // per JPEG frame
 *   avi.Finalize();                   // patches headers + writes idx1
 *   f.close();
 *
 * Layout written by Begin():
 *   RIFF/AVI -> LIST/hdrl (avih + LIST/strl(strh + strf)) -> LIST/movi
 * Frames are appended as '00dc' chunks; Finalize() appends idx1 and
 * patches the size/count fields marked with PLACEHOLDER below.
 *
 * The idx1 index is kept in RAM (16 bytes per frame: 15fps x 10min
 * ~= 144KB, well within ESP32-S3 headroom).
 */

#ifndef AVI_WRITER_H
#define AVI_WRITER_H

#include <FS.h>
#include <vector>

class AviWriter {
public:
    bool Begin(File& file, uint16_t width, uint16_t height, uint16_t fps) {
        f_ = &file;
        w_ = width;
        h_ = height;
        fps_ = fps;
        frame_count_ = 0;
        index_.clear();

        // ---- RIFF header (size patched in Finalize) ----
        WriteTag("RIFF"); WriteU32(0); WriteTag("AVI ");

        // ---- LIST hdrl ----
        WriteTag("LIST"); WriteU32(192); WriteTag("hdrl");

        // avih - main AVI header (56 bytes)
        WriteTag("avih"); WriteU32(56);
        WriteU32(1000000UL / fps_);     // dwMicroSecPerFrame
        WriteU32(0);                    // dwMaxBytesPerSec (unknown)
        WriteU32(0);                    // dwPaddingGranularity
        WriteU32(0x10);                 // dwFlags: AVIF_HASINDEX
        avih_total_frames_pos_ = f_->position();
        WriteU32(0);                    // dwTotalFrames (patched)
        WriteU32(0);                    // dwInitialFrames
        WriteU32(1);                    // dwStreams
        WriteU32(0);                    // dwSuggestedBufferSize
        WriteU32(w_);                   // dwWidth
        WriteU32(h_);                   // dwHeight
        WriteU32(0); WriteU32(0); WriteU32(0); WriteU32(0);   // dwReserved

        // ---- LIST strl ----
        WriteTag("LIST"); WriteU32(116); WriteTag("strl");

        // strh - video stream header (56 bytes)
        WriteTag("strh"); WriteU32(56);
        WriteTag("vids");               // fccType
        WriteTag("MJPG");               // fccHandler
        WriteU32(0);                    // dwFlags
        WriteU32(0);                    // wPriority + wLanguage
        WriteU32(0);                    // dwInitialFrames
        WriteU32(1);                    // dwScale
        WriteU32(fps_);                 // dwRate -> fps
        WriteU32(0);                    // dwStart
        strh_length_pos_ = f_->position();
        WriteU32(0);                    // dwLength = frame count (patched)
        WriteU32(0);                    // dwSuggestedBufferSize
        WriteU32(0xFFFFFFFF);           // dwQuality = default
        WriteU32(0);                    // dwSampleSize
        WriteU16(0); WriteU16(0);       // rcFrame left/top
        WriteU16(w_); WriteU16(h_);     // rcFrame right/bottom

        // strf - BITMAPINFOHEADER (40 bytes)
        WriteTag("strf"); WriteU32(40);
        WriteU32(40);                   // biSize
        WriteU32(w_);                   // biWidth
        WriteU32(h_);                   // biHeight
        WriteU16(1);                    // biPlanes
        WriteU16(24);                   // biBitCount
        WriteTag("MJPG");               // biCompression
        WriteU32((uint32_t)w_ * h_ * 3);// biSizeImage
        WriteU32(0); WriteU32(0);       // biXPelsPerMeter / biYPelsPerMeter
        WriteU32(0); WriteU32(0);       // biClrUsed / biClrImportant

        // ---- LIST movi (size patched in Finalize) ----
        WriteTag("LIST");
        movi_size_pos_ = f_->position();
        WriteU32(0);                    // movi list size (patched)
        WriteTag("movi");
        movi_data_pos_ = f_->position();

        return f_->position() > 0;
    }

    bool AddFrame(const uint8_t* jpeg, size_t len) {
        if (!f_ || len == 0) return false;

        // idx1 entry: offset relative to the start of the movi data
        index_.push_back((uint32_t)(f_->position() - movi_data_pos_));
        index_.push_back((uint32_t)len);

        WriteTag("00dc");
        WriteU32((uint32_t)len);
        size_t written = f_->write(jpeg, len);
        if (len & 1) f_->write((uint8_t)0);     // chunks are word-aligned
        frame_count_++;
        return written == len;
    }

    bool Finalize() {
        if (!f_) return false;

        // ---- idx1 index ----
        WriteTag("idx1");
        WriteU32((uint32_t)index_.size() * 4);      // 16 bytes per frame
        for (size_t i = 0; i + 1 < index_.size(); i += 2) {
            WriteTag("00dc");
            WriteU32(0x10);             // AVIIF_KEYFRAME
            WriteU32(index_[i]);        // offset (relative to movi data)
            WriteU32(index_[i + 1]);    // size
        }

        uint32_t file_size = f_->position();

        // ---- patch sizes and frame counts ----
        PatchU32(4, file_size - 8);                         // RIFF size
        PatchU32(avih_total_frames_pos_, frame_count_);     // avih dwTotalFrames
        PatchU32(strh_length_pos_, frame_count_);           // strh dwLength
        // movi LIST payload = 'movi' fourcc + all frame chunks (everything
        // between movi_data_pos_ and the start of idx1)
        PatchU32(movi_size_pos_, 4 + (file_size - idx1Size()) - movi_data_pos_);
        f_->flush();
        return true;
    }

    uint32_t Frames() const { return frame_count_; }

private:
    // Total size of the idx1 chunk (header + payload) as written by Finalize()
    uint32_t idx1Size() const {
        return 8 + frame_count_ * 16;
    }

    void WriteTag(const char* tag) { f_->write((const uint8_t*)tag, 4); }

    void WriteU16(uint16_t v) {
        uint8_t b[2] = { (uint8_t)(v & 0xFF), (uint8_t)(v >> 8) };
        f_->write(b, 2);
    }

    void WriteU32(uint32_t v) {
        uint8_t b[4] = { (uint8_t)(v & 0xFF), (uint8_t)((v >> 8) & 0xFF),
                         (uint8_t)((v >> 16) & 0xFF), (uint8_t)((v >> 24) & 0xFF) };
        f_->write(b, 4);
    }

    void PatchU32(size_t pos, uint32_t v) {
        size_t cur = f_->position();
        f_->seek(pos);
        WriteU32(v);
        f_->seek(cur);
    }

    File* f_ = nullptr;
    uint16_t w_ = 0, h_ = 0, fps_ = 15;
    uint32_t frame_count_ = 0;
    size_t movi_data_pos_ = 0;
    size_t movi_size_pos_ = 0;
    size_t avih_total_frames_pos_ = 0;
    size_t strh_length_pos_ = 0;
    std::vector<uint32_t> index_;    // offset/size pairs, one pair per frame
};

#endif /* AVI_WRITER_H */
