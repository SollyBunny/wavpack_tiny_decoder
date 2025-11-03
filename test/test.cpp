#include "../wavpack.h"

#include <algorithm>

#include <cerrno>
#include <cstdio>
#include <cstring>

#define log_error(_, fmt, ...) printf(fmt "\n", ##__VA_ARGS__)
#define log_warn(_, fmt, ...) printf(fmt "\n", ##__VA_ARGS__)
#define log_info(_, fmt, ...) printf(fmt "\n", ##__VA_ARGS__)

#define mem_copy memcpy

bool str_toint(const char *str, int *out)
{
	// returns true if conversion was successful
	char *end;
	int value = strtol(str, &end, 10);
	if(*end != '\0')
		return false;
	if(out != nullptr)
		*out = value;
	return true;
}

struct CSample
{
	int m_Index;
	int m_NextFreeSampleIndex;

	short *m_pData;
	int m_NumFrames;
	int m_Rate;
	int m_Channels;
	int m_LoopStart;
	int m_PausedAt;

	float TotalTime() const
	{
		return m_NumFrames / (float)m_Rate;
	}

	bool IsLoaded() const
	{
		return m_pData != nullptr;
	}
};

static bool DecodeWV(CSample &Sample, const void *pData, unsigned DataSize, const char *pContextName)
{
	char aError[100];

	struct {
		const char *m_pContextName;
		const void *m_pData;
		uint32_t m_Position;
		uint32_t m_Length;
	} BufferData = {pContextName, pData, 0, DataSize};

	WavpackStreamReader Callback = {
		.read_bytes = [](void *pId, void *pBuffer, int32_t Size) {
			auto& CallbackData1 = *(decltype(BufferData) *)pId;
			int ChunkSize = std::min<int>(Size, CallbackData1.m_Length - CallbackData1.m_Position);
			mem_copy(pBuffer, (const char *)CallbackData1.m_pData + CallbackData1.m_Position, ChunkSize);
			CallbackData1.m_Position += ChunkSize;
			return ChunkSize;
		},
		.get_pos = [](void *pId) {
			return ((decltype(BufferData) *)pId)->m_Position;
		},
		.set_pos_abs = [](void *pId, uint32_t Pos) -> int32_t {
			return ((decltype(BufferData) *)pId)->m_Position = Pos;
		},
		.set_pos_rel = [](void *pId, int32_t Offset, int Whence) -> int32_t {
			auto& CallbackData1 = *(decltype(BufferData) *)pId;
			if(Whence == SEEK_SET)
				CallbackData1.m_Position = Offset;
			else if(Whence == SEEK_CUR)
				CallbackData1.m_Position += Offset;
			else if(Whence == SEEK_END)
				CallbackData1.m_Position = CallbackData1.m_Length + Offset;
			else {
				log_error("sound/wv", "Wavpack tried to seek with an unknown type. Offset=%d, Whence=%d, Filename='%s'", Offset, Whence, CallbackData1.m_pContextName);
				return -1;
			}
			CallbackData1.m_Position = std::clamp<uint32_t>(CallbackData1.m_Position, 0, CallbackData1.m_Length);
			return CallbackData1.m_Position;
		},
		.push_back_byte = [](void *pId, int Char) {
			((decltype(BufferData) *)pId)->m_Position -= 1;
			(void)Char; // no-op
			return 0;
		},
		.get_length = [](void *pId) {
			return ((decltype(BufferData) *)pId)->m_Length;
		},
		.can_seek = [](void *) { return (int)true; },
		.write_bytes = [](void *pId, void *pBuffer, int Length) {
			((decltype(BufferData) *)pId)->m_Position += Length;
			(void)pBuffer; // no-op
			return 0;
		},
	};
	WavpackContext *pContext = WavpackOpenFileInputEx(&Callback, (void *)&BufferData, 0, aError, OPEN_TAGS, 0);
	if(pContext)
	{
		const int NumSamples = WavpackGetNumSamples(pContext);
		const int BitsPerSample = WavpackGetBitsPerSample(pContext);
		const unsigned int SampleRate = WavpackGetSampleRate(pContext);
		const int NumChannels = WavpackGetNumChannels(pContext);

		if(NumChannels > 2)
		{
			log_error("sound/wv", "File is not mono or stereo. Filename='%s'", pContextName);
			return false;
		}

		if(BitsPerSample != 16)
		{
			log_error("sound/wv", "Bits per sample is %d, not 16. Filename='%s'", BitsPerSample, pContextName);
			return false;
		}

		int *pBuffer = (int *)calloc((size_t)NumSamples * NumChannels, sizeof(int));
		if(!WavpackUnpackSamples(pContext, pBuffer, NumSamples))
		{
			free(pBuffer);
			log_error("sound/wv", "WavpackUnpackSamples failed. NumSamples=%d NumChannels=%d Filename='%s'", NumSamples, NumChannels, pContextName);
			return false;
		}

		Sample.m_pData = (short *)calloc((size_t)NumSamples * NumChannels, sizeof(short));

		int *pSrc = pBuffer;
		short *pDst = Sample.m_pData;
		for(int i = 0; i < NumSamples * NumChannels; i++)
			*pDst++ = (short)*pSrc++;

		free(pBuffer);

		Sample.m_NumFrames = NumSamples;
		Sample.m_Rate = SampleRate;
		Sample.m_Channels = NumChannels;
		Sample.m_LoopStart = 0;
		Sample.m_PausedAt = 0;

		char aBuf[128];
		if(WavpackGetTagItem(pContext, "loop_start", aBuf, sizeof(aBuf)) > 0)
		{
			if(!str_toint(aBuf, &Sample.m_LoopStart)) {
				log_error("sound/wv", "Failed to parse loop_start tag. Value='%s', Filename='%s'", aBuf, pContextName);
				Sample.m_LoopStart = -1;
			}
			else if(Sample.m_LoopStart < 1 || Sample.m_LoopStart >= Sample.m_NumFrames)
			{
				log_error("sound/wv", "Tag loop_start is out of bounds. Value=%d, Min=1, Max=%d, Filename='%s'", Sample.m_LoopStart, Sample.m_NumFrames - 1, pContextName);
				Sample.m_LoopStart = -1;
			}
		}

		WavpackCloseFile(pContext);
	}
	else
	{
		log_error("sound/wv", "Failed to decode sample (%s). Filename='%s'", aError, pContextName);
		return false;
	}

	return true;
}

int main(int argc, char **argv) {
	// Args
	if(argc != 2)
	{
		std::printf("usage: %s [music.wav]", argv[0]);
		return 0;
	}

	// Read file
	auto CheckErrno = [](const char *pWhat) {
		if(errno != 0)
		{
			std::perror(pWhat);
			std::exit(1);
		}
	};

	auto File = std::fopen(argv[1], "rb");
	CheckErrno("fopen %1");
	std::fseek(File, 0, SEEK_END);
	CheckErrno("fseek SEEK_END");
	auto Size = std::ftell(File);
	CheckErrno("ftell %1");
	std::fseek(File, 0, SEEK_SET);
	CheckErrno("fseek SEEK_SET");
	auto Data = std::malloc(Size);
	std::fread(Data, 1, Size, File);
	CheckErrno("fread %1");
	std::fclose(File);

	std::printf("Read %s (%d bytes)\n", argv[1], (int)Size);

	// Parse
	CSample Sample;
	if(!DecodeWV(Sample, Data, Size, "test"))
	{
		std::printf("Decode failed\n");
		return 1;
	}
	std::printf("Decoded %d frames at %dhz (%f seconds)\n", Sample.m_NumFrames, Sample.m_Rate, (float)Sample.m_NumFrames / (float)Sample.m_Rate);
	
	std::free(Data);
	
	File = std::fopen("out.wav", "wb");
	CheckErrno("fopen out.wav");
	
	// Do looping
	if(Sample.m_LoopStart >= 0)
	{
		std::printf("Loop pt: %d (%f)\n", Sample.m_LoopStart, (float)Sample.m_LoopStart / (float)Sample.m_Rate);
		int LoopLen = Sample.m_NumFrames - Sample.m_LoopStart;
		int LoopCount = 50;
		Sample.m_pData = (short *)realloc(Sample.m_pData, (Sample.m_NumFrames + LoopLen * LoopCount) * sizeof(short) * Sample.m_Channels);
		for(int i = 0; i < LoopCount; i++)
		{
			std::memcpy(
				Sample.m_pData + (Sample.m_NumFrames + i * LoopLen) * Sample.m_Channels,
				Sample.m_pData + Sample.m_LoopStart * Sample.m_Channels,
				LoopLen * Sample.m_Channels * sizeof(short)
			);
		}
		Sample.m_NumFrames += LoopLen * LoopCount;
	}

	// WAV header fields
	uint32_t data_size = Sample.m_NumFrames * Sample.m_Channels * sizeof(short);
	uint32_t chunk_size = 36 + data_size;
	uint16_t audio_format = 1;  // PCM
	uint16_t bits_per_sample = 16;
	uint32_t byte_rate = Sample.m_Rate * Sample.m_Channels * bits_per_sample / 8;
	uint16_t block_align = Sample.m_Channels * bits_per_sample / 8;

	// Write WAV header
	fwrite("RIFF", 1, 4, File);
	fwrite(&chunk_size, 4, 1, File);
	fwrite("WAVEfmt ", 1, 8, File);
	uint32_t subchunk1_size = 16;
	fwrite(&subchunk1_size, 4, 1, File);
	fwrite(&audio_format, 2, 1, File);
	fwrite(&Sample.m_Channels, 2, 1, File);
	fwrite(&Sample.m_Rate, 4, 1, File);
	fwrite(&byte_rate, 4, 1, File);
	fwrite(&block_align, 2, 1, File);
	fwrite(&bits_per_sample, 2, 1, File);
	fwrite("data", 1, 4, File);
	fwrite(&data_size, 4, 1, File);

	// Write PCM data
	fwrite(Sample.m_pData, sizeof(short), Sample.m_NumFrames * Sample.m_Channels, File);

	CheckErrno("fwrite out.wav");

	fclose(File);

	free(Sample.m_pData);

	printf("Done!\n");
}

// #endif
