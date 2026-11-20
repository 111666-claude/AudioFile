#include "doctest/doctest.h"
#include <AudioFile.h>

#include <cstdint>
#include <cstdio>
#include <fstream>
#include <random>
#include <string>
#include <sys/stat.h>
#include <vector>

#if defined (__linux__)
#include <dlfcn.h>
#include <unistd.h>
#endif

//=============================================================
// Access to the optional LD_PRELOAD read() counter (read_counter.cpp).
// When the preload library is present, tests can assert the exact number of
// bytes pulled from the storage layer; otherwise the counters report zero and
// byte-amount assertions are skipped (sparseness itself is still checked).
namespace ReadCounter
{
    using SetCountingFn = void (*) (int);
    using GetReadBytesFn = uint64_t (*) (void);

    static SetCountingFn setCounting = nullptr;
    static GetReadBytesFn getReadBytes = nullptr;
    static bool initialised = false;
    static bool available = false;

    static void initialise()
    {
        if (initialised)
            return;

        initialised = true;

#if defined (__linux__)
        void* handle = dlopen (nullptr, RTLD_NOW | RTLD_GLOBAL);

        if (handle != nullptr)
        {
            setCounting = reinterpret_cast<SetCountingFn> (dlsym (handle, "audiofile_test_set_counting"));
            getReadBytes = reinterpret_cast<GetReadBytesFn> (dlsym (handle, "audiofile_test_get_read_bytes"));
            available = (setCounting != nullptr && getReadBytes != nullptr);
        }
#endif
    }

    static bool isAvailable()
    {
        initialise();
        return available;
    }

    static void start()
    {
        initialise();

        if (available)
            setCounting (1);
    }

    static uint64_t stop()
    {
        if (! available)
            return 0;

        uint64_t bytes = getReadBytes();
        setCounting (0);
        return bytes;
    }
}

//=============================================================
static uint64_t getAllocatedBlocks (const std::string& filePath)
{
    struct stat fileStat;
    REQUIRE (stat (filePath.c_str(), &fileStat) == 0);
    return static_cast<uint64_t> (fileStat.st_blocks);
}

//=============================================================
const std::string projectBuildDirectory = PROJECT_BINARY_DIR;
const std::string sectionTestDirectory = projectBuildDirectory + "/section-tests/";

//=============================================================
// Creates an audio file with deterministic pseudo-random samples so that
// every frame has a distinct value
template <typename T>
void createTestAudioFile (const std::string& filePath, int numChannels, int sampleRate, int bitDepth,
                          int numSamplesPerChannel, AudioFileFormat format, uint32_t seed = 12345)
{
    AudioFile<T> audioFile;
    audioFile.setAudioBufferSize (numChannels, numSamplesPerChannel);
    audioFile.setSampleRate (static_cast<uint32_t> (sampleRate));
    audioFile.setBitDepth (bitDepth);

    std::mt19937 rng (seed);

    for (int i = 0; i < numSamplesPerChannel; i++)
    {
        for (int k = 0; k < numChannels; k++)
        {
            if constexpr (std::is_floating_point_v<T>)
            {
                // keep values well away from the +/-1 clipping limits
                audioFile.samples[k][i] = static_cast<T> (std::uniform_real_distribution<double> (-0.9, 0.9) (rng));
            }
            else if constexpr (std::is_signed_v<T>)
            {
                audioFile.samples[k][i] = static_cast<T> (std::uniform_int_distribution<int> (-20000, 20000) (rng));
            }
            else
            {
                audioFile.samples[k][i] = static_cast<T> (std::uniform_int_distribution<int> (1000, 55000) (rng));
            }
        }
    }

    REQUIRE (audioFile.save (filePath, format));
}

//=============================================================
// Compares a section load against a full load followed by slicing, sample by sample
template <typename T>
void checkSectionMatchesFullLoad (const std::string& filePath, int64_t startSample, int64_t numSamples,
                                  bool truncate, int expectedSamplesReturned)
{
    AudioFile<T> fullLoad;
    REQUIRE (fullLoad.load (filePath));

    AudioFile<T> sectionLoad;
    sectionLoad.shouldLogErrorsToConsole (false);
    bool ok = sectionLoad.loadSection (filePath, startSample, numSamples, truncate);
    REQUIRE (ok);

    // the metadata must describe the file, not the section length
    CHECK_EQ (sectionLoad.getSampleRate(), fullLoad.getSampleRate());
    CHECK_EQ (sectionLoad.getNumChannels(), fullLoad.getNumChannels());
    CHECK_EQ (sectionLoad.getBitDepth(), fullLoad.getBitDepth());
    CHECK_EQ (sectionLoad.getNumSamplesPerChannel(), expectedSamplesReturned);

    for (int channel = 0; channel < fullLoad.getNumChannels(); channel++)
    {
        for (int i = 0; i < expectedSamplesReturned; i++)
        {
            CHECK (sectionLoad.samples[channel][i] == fullLoad.samples[channel][startSample + i]);
        }
    }
}

//=============================================================
static std::string sectionFilePath (const std::string& name)
{
    return sectionTestDirectory + name;
}

//=============================================================
static void writeBytes (const std::string& filePath, const std::vector<uint8_t>& bytes)
{
    std::ofstream out (filePath, std::ios::binary | std::ios::trunc);
    REQUIRE (out.is_open());
    out.write (reinterpret_cast<const char*> (bytes.data()), static_cast<std::streamsize> (bytes.size()));
}

//=============================================================
static void appendInt32 (std::vector<uint8_t>& bytes, int32_t value, bool littleEndian)
{
    for (int i = 0; i < 4; i++)
    {
        int shift = littleEndian ? (i * 8) : ((3 - i) * 8);
        bytes.push_back (static_cast<uint8_t> ((value >> shift) & 0xFF));
    }
}

static void appendInt16 (std::vector<uint8_t>& bytes, int16_t value, bool littleEndian)
{
    if (littleEndian)
    {
        bytes.push_back (static_cast<uint8_t> (value & 0xFF));
        bytes.push_back (static_cast<uint8_t> ((value >> 8) & 0xFF));
    }
    else
    {
        bytes.push_back (static_cast<uint8_t> ((value >> 8) & 0xFF));
        bytes.push_back (static_cast<uint8_t> (value & 0xFF));
    }
}

static void appendString (std::vector<uint8_t>& bytes, const std::string& s)
{
    for (char c : s)
        bytes.push_back (static_cast<uint8_t> (c));
}

//=============================================================
// Builds a WAVE_FORMAT_EXTENSIBLE file from raw PCM bytes, with a custom
// chunk placed before the data chunk
static void writeExtensibleWav (const std::string& filePath, uint16_t numChannels, uint32_t sampleRate,
                                uint16_t bitDepth, const std::vector<uint8_t>& pcmData,
                                uint16_t subFormatCode, bool addUnknownChunk)
{
    std::vector<uint8_t> bytes;
    uint16_t blockAlign = static_cast<uint16_t> (numChannels * (bitDepth / 8));
    uint32_t byteRate = sampleRate * blockAlign;
    int32_t fmtChunkSize = 40; // WAVE_FORMAT_EXTENSIBLE uses a 40-byte fmt chunk
    int32_t dataChunkSize = static_cast<int32_t> (pcmData.size());

    int32_t riffSize = 4 + (8 + fmtChunkSize) + (8 + dataChunkSize);

    if (addUnknownChunk)
        riffSize += (8 + 6);

    appendString (bytes, "RIFF");
    appendInt32 (bytes, riffSize, true);
    appendString (bytes, "WAVE");

    appendString (bytes, "fmt ");
    appendInt32 (bytes, fmtChunkSize, true);
    appendInt16 (bytes, static_cast<int16_t> (WavAudioFormat::Extensible), true); // audio format
    appendInt16 (bytes, static_cast<int16_t> (numChannels), true);
    appendInt32 (bytes, static_cast<int32_t> (sampleRate), true);
    appendInt32 (bytes, static_cast<int32_t> (byteRate), true);
    appendInt16 (bytes, static_cast<int16_t> (blockAlign), true);
    appendInt16 (bytes, static_cast<int16_t> (bitDepth), true);
    appendInt16 (bytes, 22, true);  // extension size
    appendInt16 (bytes, bitDepth, true); // valid bits per sample
    appendInt32 (bytes, 0, true);   // channel mask
    appendInt16 (bytes, static_cast<int16_t> (subFormatCode), true); // sub-format code
    // KSDATAFORMAT_SUBTYPE_PCM / IEEE_FLOAT GUID remainder
    for (uint8_t b : { 0x00, 0x00, 0x00, 0x00, 0x10, 0x00, 0x80, 0x00, 0x00, 0xaa, 0x00, 0x38, 0x9b, 0x71 })
        bytes.push_back (b);

    if (addUnknownChunk)
    {
        appendString (bytes, "xtra");
        appendInt32 (bytes, 6, true);

        for (uint8_t b : { 1, 2, 3, 4, 5, 6 })
            bytes.push_back (b);
    }

    appendString (bytes, "data");
    appendInt32 (bytes, dataChunkSize, true);
    bytes.insert (bytes.end(), pcmData.begin(), pcmData.end());

    writeBytes (filePath, bytes);
}

//=============================================================
TEST_SUITE ("RangeLoadingTests - WAV parity with full load and slicing")
{
    struct WavCase
    {
        const char* name;
        int numChannels;
        int sampleRate;
        int bitDepth;
    };

    TEST_CASE ("RangeLoadingTests::WavAllBitDepthsAndChannelCounts")
    {
        const WavCase cases[] = {
            { "wav_mono_8bit.wav",    1, 44100, 8  },
            { "wav_stereo_8bit.wav",  2, 44100, 8  },
            { "wav_mono_16bit.wav",   1, 48000, 16 },
            { "wav_stereo_16bit.wav", 2, 48000, 16 },
            { "wav_stereo_24bit.wav", 2, 44100, 24 },
            { "wav_8chan_24bit.wav",  8, 48000, 24 },
            { "wav_stereo_32bit.wav", 2, 48000, 32 }
        };

        for (const auto& testCase : cases)
        {
            const std::string filePath = sectionFilePath (testCase.name);
            createTestAudioFile<double> (filePath, testCase.numChannels, testCase.sampleRate, testCase.bitDepth, 1000, AudioFileFormat::Wave);

            CAPTURE (testCase.name);

            // a section wholly inside the file
            checkSectionMatchesFullLoad<double> (filePath, 0, 10, true, 10);
            checkSectionMatchesFullLoad<double> (filePath, 37, 13, true, 13);
            checkSectionMatchesFullLoad<double> (filePath, 990, 10, true, 10);

            // reading the whole file as a section
            checkSectionMatchesFullLoad<double> (filePath, 0, 1000, true, 1000);
        }
    }

    TEST_CASE ("RangeLoadingTests::WavTruncationAtEndOfFile")
    {
        const std::string filePath = sectionFilePath ("wav_truncation.wav");
        createTestAudioFile<double> (filePath, 2, 44100, 16, 100, AudioFileFormat::Wave);

        // a request crossing the end is truncated by default, matching full-load slicing
        checkSectionMatchesFullLoad<double> (filePath, 90, 50, true, 10);

        // with truncation disabled, the same request fails
        AudioFile<double> sectionLoad;
        sectionLoad.shouldLogErrorsToConsole (false);
        CHECK (! sectionLoad.loadSection (filePath, 90, 50, false));

        // a start sample exactly at the end yields no frames and must fail
        CHECK (! sectionLoad.loadSection (filePath, 100, 1, true));
        CHECK (! sectionLoad.loadSection (filePath, 1000, 1, true));
    }

    TEST_CASE ("RangeLoadingTests::WavInvalidArguments")
    {
        const std::string filePath = sectionFilePath ("wav_invalid_args.wav");
        createTestAudioFile<double> (filePath, 2, 44100, 16, 100, AudioFileFormat::Wave);

        AudioFile<double> audioFile;
        audioFile.shouldLogErrorsToConsole (false);

        CHECK (! audioFile.loadSection (filePath, -1, 10));
        CHECK (! audioFile.loadSection (filePath, 0, 0));
        CHECK (! audioFile.loadSection (filePath, 5, -10));
        CHECK (! audioFile.loadSection (filePath, std::numeric_limits<int64_t>::max(), 1));
        CHECK (! audioFile.loadSection (filePath, 0, std::numeric_limits<int64_t>::max()));
        CHECK (! audioFile.loadSection (sectionFilePath ("this_file_does_not_exist.wav"), 0, 10));
    }

    TEST_CASE ("RangeLoadingTests::WavStateIsUntouchedOnFailure")
    {
        const std::string filePath = sectionFilePath ("wav_state_untouched.wav");
        createTestAudioFile<double> (filePath, 2, 44100, 16, 100, AudioFileFormat::Wave);

        AudioFile<double> audioFile;
        REQUIRE (audioFile.loadSection (filePath, 10, 20));
        CHECK_EQ (audioFile.getNumSamplesPerChannel(), 20);

        audioFile.shouldLogErrorsToConsole (false);
        CHECK (! audioFile.loadSection (filePath, 500, 10, false));

        // the previous section must still be intact
        CHECK_EQ (audioFile.getNumSamplesPerChannel(), 20);
        AudioFile<double> reference;
        REQUIRE (reference.load (filePath));

        for (int channel = 0; channel < 2; channel++)
            for (int i = 0; i < 20; i++)
                CHECK (audioFile.samples[channel][i] == reference.samples[channel][10 + i]);
    }

    TEST_CASE ("RangeLoadingTests::WavExtensibleAndUnknownChunks")
    {
        // 2 channels, 48kHz, 16-bit, 4 frames of PCM
        std::vector<uint8_t> pcm = {
            0x00, 0x00, 0x00, 0x80,   // frame 0: ch0 = 0,      ch1 = -32768
            0xff, 0x7f, 0x00, 0x00,   // frame 1: ch0 = 32767,  ch1 = 0
            0x00, 0x40, 0x00, (uint8_t)0xc0, // frame 2
            (uint8_t)0xff, 0x7f, 0x01, 0x00  // frame 3
        };

        const std::string filePath = sectionFilePath ("wav_extensible.wav");
        writeExtensibleWav (filePath, 2, 48000, 16, pcm, WavAudioFormat::PCM, true);

        AudioFile<double> fullLoad;
        REQUIRE (fullLoad.load (filePath));
        CHECK_EQ (fullLoad.getNumChannels(), 2);
        CHECK_EQ (fullLoad.getSampleRate(), 48000);

        // the section loader must find fmt and data despite the extension header
        // and the 'xtra' chunk sitting between them
        checkSectionMatchesFullLoad<double> (filePath, 1, 2, true, 2);
        checkSectionMatchesFullLoad<double> (filePath, 0, 4, true, 4);
    }

    TEST_CASE ("RangeLoadingTests::WavOddSizedChunkPadding")
    {
        // RIFF chunks are word-aligned, so an odd-sized chunk is followed by a
        // single pad byte. The section loader's chunk walk must skip that pad.
        std::vector<uint8_t> bytes;
        appendString (bytes, "RIFF");
        appendInt32 (bytes, 4 + (8 + 16) + (8 + 5) + 1 + (8 + 8), true);
        appendString (bytes, "WAVE");

        appendString (bytes, "fmt ");
        appendInt32 (bytes, 16, true);
        appendInt16 (bytes, 1, true);                 // PCM mono
        appendInt16 (bytes, 1, true);
        appendInt32 (bytes, 44100, true);
        appendInt32 (bytes, 88200, true);
        appendInt16 (bytes, 2, true);                 // block align
        appendInt16 (bytes, 16, true);

        appendString (bytes, "junk");
        appendInt32 (bytes, 5, true);                 // odd size -> one pad byte follows
        bytes.insert (bytes.end(), { 1, 2, 3, 4, 5, 0xAA });

        appendString (bytes, "data");
        appendInt32 (bytes, 8, true);

        // four known mono 16-bit frames: 0, 1, 2, 3
        bytes.insert (bytes.end(), { 0x00, 0x00, 0x01, 0x00, 0x02, 0x00, 0x03, 0x00 });

        const std::string filePath = sectionFilePath ("wav_odd_chunk.wav");
        writeBytes (filePath, bytes);

        AudioFile<double> section;
        section.shouldLogErrorsToConsole (false);
        REQUIRE (section.loadSection (filePath, 1, 2, true));
        CHECK_EQ (section.getNumSamplesPerChannel(), 2);
        CHECK_EQ (section.getSampleRate(), 44100);
        CHECK_EQ (section.getBitDepth(), 16);
        CHECK_EQ (section.samples[0][0], AudioSampleConverter<double>::sixteenBitIntToSample (static_cast<int16_t> (1)));
        CHECK_EQ (section.samples[0][1], AudioSampleConverter<double>::sixteenBitIntToSample (static_cast<int16_t> (2)));
    }

    TEST_CASE ("RangeLoadingTests::WavFloat32Section")
    {
        const std::string filePath = sectionFilePath ("wav_float32.wav");
        createTestAudioFile<float> (filePath, 2, 48000, 32, 256, AudioFileFormat::Wave);

        AudioFile<float> fullLoad;
        REQUIRE (fullLoad.load (filePath));
        CHECK_EQ (fullLoad.getBitDepth(), 32);

        checkSectionMatchesFullLoad<float> (filePath, 100, 50, true, 50);
    }

    TEST_CASE ("RangeLoadingTests::WavTruncatedAndCorruptFilesAreRejected")
    {
        const std::string goodFile = sectionFilePath ("wav_corrupt_base.wav");
        createTestAudioFile<double> (goodFile, 2, 44100, 16, 50, AudioFileFormat::Wave);

        AudioFile<double> audioFile;
        audioFile.shouldLogErrorsToConsole (false);

        // a truncated copy of the file (header and data cut short)
        {
            std::ifstream in (goodFile, std::ios::binary);
            std::vector<uint8_t> allBytes ((std::istreambuf_iterator<char> (in)), std::istreambuf_iterator<char>());
            allBytes.resize (allBytes.size() - 7); // cut through the final frame
            writeBytes (sectionFilePath ("wav_truncated_copy.wav"), allBytes);

            CHECK (! audioFile.loadSection (sectionFilePath ("wav_truncated_copy.wav"), 0, 10));
        }

        // a data chunk whose declared size is not a whole number of frames
        {
            std::ifstream in (goodFile, std::ios::binary);
            std::vector<uint8_t> allBytes ((std::istreambuf_iterator<char> (in)), std::istreambuf_iterator<char>());

            // find the data chunk and lie about its size
            for (size_t i = 12; i + 8 <= allBytes.size(); i += 4)
            {
                if (std::string (reinterpret_cast<char*> (&allBytes[i]), 4) == "data")
                {
                    int32_t declaredSize = static_cast<int32_t> (allBytes.size() - i - 8) + 1;
                    allBytes[i + 4] = static_cast<uint8_t> (declaredSize & 0xFF);
                    allBytes[i + 5] = static_cast<uint8_t> ((declaredSize >> 8) & 0xFF);
                    allBytes[i + 6] = static_cast<uint8_t> ((declaredSize >> 16) & 0xFF);
                    allBytes[i + 7] = static_cast<uint8_t> ((declaredSize >> 24) & 0xFF);
                    break;
                }
            }

            writeBytes (sectionFilePath ("wav_bad_data_size.wav"), allBytes);
            CHECK (! audioFile.loadSection (sectionFilePath ("wav_bad_data_size.wav"), 0, 10));
        }

        // a file with no data chunk
        {
            std::vector<uint8_t> bytes;
            appendString (bytes, "RIFF");
            appendInt32 (bytes, 12, true);
            appendString (bytes, "WAVE");
            appendString (bytes, "fmt ");
            appendInt32 (bytes, 16, true);
            appendInt16 (bytes, 1, true);
            appendInt16 (bytes, 1, true);
            appendInt32 (bytes, 44100, true);
            appendInt32 (bytes, 44100, true);
            appendInt16 (bytes, 1, true);
            appendInt16 (bytes, 8, true);
            writeBytes (sectionFilePath ("wav_no_data.wav"), bytes);

            CHECK (! audioFile.loadSection (sectionFilePath ("wav_no_data.wav"), 0, 10));
        }

        // garbage / too-small files
        {
            writeBytes (sectionFilePath ("wav_garbage.wav"), { 'R', 'I', 'F', 'F', 0, 0 });
            CHECK (! audioFile.loadSection (sectionFilePath ("wav_garbage.wav"), 0, 10));
            writeBytes (sectionFilePath ("not_audio.bin"), { 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12 });
            CHECK (! audioFile.loadSection (sectionFilePath ("not_audio.bin"), 0, 10));
        }
    }
}

//=============================================================
TEST_SUITE ("RangeLoadingTests - AIFF parity with full load and slicing")
{
    struct AiffCase
    {
        const char* name;
        int numChannels;
        int sampleRate;
        int bitDepth;
    };

    TEST_CASE ("RangeLoadingTests::AiffAllBitDepthsAndChannelCounts")
    {
        const AiffCase cases[] = {
            { "aiff_mono_8bit.aif",   1, 44100, 8  },
            { "aiff_stereo_8bit.aif", 2, 44100, 8  },
            { "aiff_mono_16bit.aif",  1, 48000, 16 },
            { "aiff_stereo_16bit.aif",2, 48000, 16 },
            { "aiff_stereo_24bit.aif",2, 44100, 24 },
            { "aiff_stereo_32bit.aif",2, 48000, 32 }
        };

        for (const auto& testCase : cases)
        {
            const std::string filePath = sectionFilePath (testCase.name);
            createTestAudioFile<double> (filePath, testCase.numChannels, testCase.sampleRate, testCase.bitDepth, 1000, AudioFileFormat::Aiff);

            CAPTURE (testCase.name);

            // big-endian sample data must decode identically to a full load
            checkSectionMatchesFullLoad<double> (filePath, 0, 10, true, 10);
            checkSectionMatchesFullLoad<double> (filePath, 42, 10, true, 10);
            checkSectionMatchesFullLoad<double> (filePath, 995, 5, true, 5);
            checkSectionMatchesFullLoad<double> (filePath, 0, 1000, true, 1000);
        }
    }

    TEST_CASE ("RangeLoadingTests::AiffTruncationAndInvalidArguments")
    {
        const std::string filePath = sectionFilePath ("aiff_truncation.aif");
        createTestAudioFile<double> (filePath, 2, 44100, 16, 100, AudioFileFormat::Aiff);

        checkSectionMatchesFullLoad<double> (filePath, 95, 100, true, 5);

        AudioFile<double> audioFile;
        audioFile.shouldLogErrorsToConsole (false);
        CHECK (! audioFile.loadSection (filePath, 95, 100, false));
        CHECK (! audioFile.loadSection (filePath, 100, 1));
        CHECK (! audioFile.loadSection (filePath, -1, 10));
        CHECK (! audioFile.loadSection (filePath, 0, 0));
        CHECK (! audioFile.loadSection (filePath, 0, std::numeric_limits<int64_t>::max()));
    }

    TEST_CASE ("RangeLoadingTests::AiffWithNonZeroSsndOffsetIsRejectedLikeFullLoad")
    {
        // The library's full loader validates (ssndChunkSize - 8) == frame bytes,
        // which never holds when the SSND offset is non-zero, so such files are
        // rejected before the offset is ever applied. The section loader must
        // behave identically rather than silently accepting a layout the rest
        // of the library does not support.
        const int numFrames = 3;

        std::vector<uint8_t> bytes;
        const int32_t ssndPayloadSize = 8 + 2 + (numFrames * 2 * 2); // 2 offset bytes + PCM
        const int32_t formSize = 4 + (8 + 18) + (8 + ssndPayloadSize);

        appendString (bytes, "FORM");
        appendInt32 (bytes, formSize, false);
        appendString (bytes, "AIFF");

        appendString (bytes, "COMM");
        appendInt32 (bytes, 18, false);
        appendInt16 (bytes, 2, false);                 // stereo
        appendInt32 (bytes, numFrames, false);        // frames per channel
        appendInt16 (bytes, 16, false);               // bit depth
        // 80-bit extended encoding of 48000 Hz
        for (uint8_t b : { 0x40, 0x0e, 0xbb, 0x80, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00 })
            bytes.push_back (b);

        appendString (bytes, "SSND");
        appendInt32 (bytes, ssndPayloadSize, false);
        appendInt32 (bytes, 2, false);                 // non-zero offset
        appendInt32 (bytes, 0, false);                 // block size
        bytes.push_back (0x00);                        // leading offset pad bytes
        bytes.push_back (0x00);

        const int16_t frameValues[] = { 10, -20, 30, -40, 50, -60 };

        for (int16_t value : frameValues)
            appendInt16 (bytes, value, false);         // big-endian PCM

        const std::string filePath = sectionFilePath ("aiff_ssnd_offset.aif");
        writeBytes (filePath, bytes);

        AudioFile<double> section;
        section.shouldLogErrorsToConsole (false);
        AudioFile<double> full;
        full.shouldLogErrorsToConsole (false);

        // both loaders reject the unsupported non-zero-offset layout
        CHECK (! section.loadSection (filePath, 0, 2, true));
        CHECK (! full.load (filePath));
    }

    TEST_CASE ("RangeLoadingTests::AiffTruncatedFilesAreRejected")
    {
        const std::string goodFile = sectionFilePath ("aiff_corrupt_base.aif");
        createTestAudioFile<double> (goodFile, 2, 44100, 16, 50, AudioFileFormat::Aiff);

        AudioFile<double> audioFile;
        audioFile.shouldLogErrorsToConsole (false);

        std::ifstream in (goodFile, std::ios::binary);
        std::vector<uint8_t> allBytes ((std::istreambuf_iterator<char> (in)), std::istreambuf_iterator<char>());

        // cut the final frame in half (stereo 16-bit frames are 4 bytes)
        std::vector<uint8_t> truncated = allBytes;
        truncated.resize (truncated.size() - 2);
        writeBytes (sectionFilePath ("aiff_truncated_copy.aif"), truncated);
        CHECK (! audioFile.loadSection (sectionFilePath ("aiff_truncated_copy.aif"), 0, 10));

        // a file missing the SSND chunk
        {
            std::vector<uint8_t> bytes;
            appendString (bytes, "FORM");
            appendInt32 (bytes, 4 + 8 + 18, false);
            appendString (bytes, "AIFF");
            appendString (bytes, "COMM");
            appendInt32 (bytes, 18, false);
            appendInt16 (bytes, 1, false);
            appendInt32 (bytes, 0, false);
            appendInt16 (bytes, 16, false);
            // 10-byte 80-bit extended sample rate encoding of 44100
            for (uint8_t b : { 0x40, 0x0e, 0xac, 0x44, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00 })
                bytes.push_back (b);
            writeBytes (sectionFilePath ("aiff_no_ssnd.aif"), bytes);
            CHECK (! audioFile.loadSection (sectionFilePath ("aiff_no_ssnd.aif"), 0, 10));
        }
    }
}

//=============================================================
TEST_SUITE ("RangeLoadingTests - I/O behaviour on large sparse files")
{
#if defined (__linux__)
    TEST_CASE ("RangeLoadingTests::LargeSparseWavReadsOnlyTheRequestedBytes")
    {
        // A two-hour, 48kHz stereo 16-bit recording is ~2.76 GB of PCM. Classic
        // WAV headers use signed 32-bit chunk sizes, so here we build a sparse
        // file with 2 GB of PCM (~1.86 hours): almost no disk blocks are
        // allocated, and a section load must not touch the whole file.
        const int64_t numFrames = 500000000; // 2 GB / 4 bytes per stereo frame
        const int numChannels = 2;
        const int bitDepth = 16;
        const int blockAlign = numChannels * (bitDepth / 8);
        const std::string filePath = sectionFilePath ("large_sparse.wav");

        int64_t dataSize64 = numFrames * blockAlign; // 2,000,000,000 bytes
        int32_t dataSize = static_cast<int32_t> (dataSize64);
        int32_t riffSize = static_cast<int32_t> (4 + 8 + 16 + 8 + dataSize64);

        {
            std::ofstream out (filePath, std::ios::binary | std::ios::trunc);
            REQUIRE (out.is_open());

            std::vector<uint8_t> header;
            appendString (header, "RIFF");
            appendInt32 (header, riffSize, true);
            appendString (header, "WAVE");
            appendString (header, "fmt ");
            appendInt32 (header, 16, true);
            appendInt16 (header, 1, true);                 // PCM
            appendInt16 (header, static_cast<int16_t> (numChannels), true);
            appendInt32 (header, 48000, true);
            appendInt32 (header, 48000 * blockAlign, true);
            appendInt16 (header, static_cast<int16_t> (blockAlign), true);
            appendInt16 (header, static_cast<int16_t> (bitDepth), true);
            appendString (header, "data");
            appendInt32 (header, dataSize, true);
            out.write (reinterpret_cast<const char*> (header.data()), static_cast<std::streamsize> (header.size()));

            // punch real content into two sparse regions so the section data is non-zero
            auto writeFramePattern = [&] (int64_t frameIndex, int16_t value)
            {
                std::vector<uint8_t> frame (static_cast<size_t> (blockAlign));

                for (int c = 0; c < numChannels; c++)
                {
                    frame[c * 2] = static_cast<uint8_t> (value & 0xFF);
                    frame[c * 2 + 1] = static_cast<uint8_t> ((value >> 8) & 0xFF);
                }

                int64_t offset = 44 + frameIndex * blockAlign;
                out.seekp (static_cast<std::streamoff> (offset));
                out.write (reinterpret_cast<const char*> (frame.data()), static_cast<std::streamsize> (frame.size()));
            };

            writeFramePattern (42, 12345);
            writeFramePattern (43, -12345);
            writeFramePattern (numFrames - 3, 23456);
            writeFramePattern (numFrames - 2, -23456);

            // set the logical file size without writing the hole
            out.seekp (static_cast<std::streamoff> (static_cast<int64_t> (44) + dataSize - 1));
            char lastByte = 0;
            out.write (&lastByte, 1);
        }

        struct stat fileStat;
        REQUIRE (stat (filePath.c_str(), &fileStat) == 0);
        CHECK_EQ (fileStat.st_size, static_cast<off_t> (44) + static_cast<off_t> (dataSize));

        // the file must actually be sparse on disk
        uint64_t allocatedBlocks = getAllocatedBlocks (filePath);
        uint64_t logicalBlocks = static_cast<uint64_t> (fileStat.st_size) / 512;
        CHECK (allocatedBlocks < logicalBlocks / 100);

        const bool byteCounterAvailable = ReadCounter::isAvailable();
        INFO ("read() byte counter availability: " << byteCounterAvailable);

        // read ten seconds of audio near the start of the file, as in the use case
        const int64_t startSample = 48000 * 42 / 60; // 42nd minute
        const int64_t sectionLength = 10;

        ReadCounter::start();
        AudioFile<double> section;
        section.shouldLogErrorsToConsole (false);
        bool ok = section.loadSection (filePath, startSample, sectionLength, true);
        REQUIRE (ok);
        uint64_t sectionReadBytes = ReadCounter::stop();

        // the section load may read headers plus the PCM window (a few KB), but
        // nowhere near the whole 2 GB file
        if (byteCounterAvailable)
        {
            INFO ("section read() bytes: " << sectionReadBytes);
            CHECK (sectionReadBytes < static_cast<uint64_t> (fileStat.st_size) / 50);
            // the PCM window itself is 40 bytes (10 stereo 16-bit frames);
            // header scanning adds chunk headers, so expect a small bounded total
            CHECK (sectionReadBytes < 65536);
        }

        CHECK_EQ (section.getNumChannels(), 2);
        CHECK_EQ (section.getSampleRate(), 48000);
        CHECK_EQ (section.getBitDepth(), 16);
        CHECK_EQ (section.getNumSamplesPerChannel(), static_cast<int> (sectionLength));

        // a full load must read essentially the whole 2 GB, proving the contrast
        ReadCounter::start();
        AudioFile<double> fullLoad;
        fullLoad.shouldLogErrorsToConsole (false);
        REQUIRE (fullLoad.load (filePath));
        uint64_t fullReadBytes = ReadCounter::stop();

        if (byteCounterAvailable)
        {
            INFO ("full load read() bytes: " << fullReadBytes);
            CHECK (fullReadBytes >= static_cast<uint64_t> (fileStat.st_size) - 4096);
            CHECK (fullReadBytes > sectionReadBytes * 1000);
        }

        // and a section near the very end must also read only a few KB
        ReadCounter::start();
        AudioFile<double> tail;
        tail.shouldLogErrorsToConsole (false);
        REQUIRE (tail.loadSection (filePath, numFrames - 10, 10, true));
        uint64_t tailReadBytes = ReadCounter::stop();

        if (byteCounterAvailable)
        {
            INFO ("tail section read() bytes: " << tailReadBytes);
            CHECK (tailReadBytes < 65536);
        }

        CHECK_EQ (tail.samples[0][7], AudioSampleConverter<double>::sixteenBitIntToSample (static_cast<int16_t> (23456)));
        CHECK_EQ (tail.samples[1][8], AudioSampleConverter<double>::sixteenBitIntToSample (static_cast<int16_t> (-23456)));
    }
#else
    TEST_CASE ("RangeLoadingTests::LargeSparseFileTestSkippedOnThisPlatform")
    {
        INFO ("Sparse file I/O accounting is only checked on Linux");
    }
#endif
}
