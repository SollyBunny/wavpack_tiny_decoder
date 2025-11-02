#include "../wavpack.h"

#include <algorithm>

#include <cerrno>
#include <cstdio>
#include <cstring>

#define log_error(_, fmt, ...) printf(fmt "\n", ##__VA_ARGS__)
#define log_warn(_, fmt, ...) printf(fmt "\n", ##__VA_ARGS__)
#define log_info(_, fmt, ...) printf(fmt "\n", ##__VA_ARGS__)

#define mem_copy memcpy

struct CSample
{
	int m_Index;
	int m_NextFreeSampleIndex;

	short *m_pData;
	int m_NumFrames;
	int m_Rate;
	int m_Channels;
	int m_LoopStart;
	int m_LoopEnd;
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

bool DecodeWV(CSample &Sample, const void *pData, unsigned DataSize, const char *pContextName)
{
	char aError[100];

	struct {
		const void *m_pBuffer;
		uint32_t m_BufferPosition;
		uint32_t m_BufferSize;
	} CallbackData = {pData, 0, DataSize};

	WavpackStreamReader Callback = {0};
	Callback.can_seek = [](void *) { return (int)false; };
	Callback.get_length = [](void *pId) { return ((decltype(CallbackData) *)pId)->m_BufferSize; };
	Callback.get_pos = [](void *pId) { return ((decltype(CallbackData) *)pId)->m_BufferPosition; };
	Callback.push_back_byte = [](void *pId, int Char) {
		((decltype(CallbackData) *)pId)->m_BufferPosition -= 1;
		return 0;
	};
	Callback.read_bytes = [](void *pId, void *pBuffer, int32_t Size) {
		auto& CallbackData1 = *(decltype(CallbackData) *)pId;
		int ChunkSize = std::min<int>(Size, CallbackData1.m_BufferSize - CallbackData1.m_BufferPosition);
		mem_copy(pBuffer, (const char *)CallbackData1.m_pBuffer + CallbackData1.m_BufferPosition, ChunkSize);
		CallbackData1.m_BufferPosition += ChunkSize;
		return ChunkSize;
	};
	Callback.set_pos_abs = [](void *pId, uint32_t Pos) -> int32_t {
		return ((decltype(CallbackData) *)pId)->m_BufferPosition = Pos;
	};
	Callback.set_pos_rel = [](void *pId, int32_t Offset, int Whence) -> int32_t {
		auto& CallbackData1 = *(decltype(CallbackData) *)pId;
		if(Whence == SEEK_SET)
			return CallbackData1.m_BufferPosition = Offset;
		if(Whence == SEEK_CUR)
			return CallbackData1.m_BufferPosition += Offset;
		if(Whence == SEEK_END)
			return CallbackData1.m_BufferPosition = CallbackData1.m_BufferSize - Offset;
		return -1;
	};
	WavpackContext *pContext = WavpackOpenFileInputEx(&Callback, (void *)&CallbackData, 0, aError, 0, 0);
	if(pContext)
	{
		const int NumSamples = WavpackGetNumSamples(pContext);
		const int BitsPerSample = WavpackGetBitsPerSample(pContext);
		const unsigned int SampleRate = WavpackGetSampleRate(pContext);
		const int NumChannels = WavpackGetNumChannels(pContext);

		if(NumChannels > 2)
		{
			log_error("sound/wv", "File is not mono or stereo. Filename='%s'", pContextName);
			CallbackData.m_pBuffer = nullptr;
			return false;
		}

		if(BitsPerSample != 16)
		{
			log_error("sound/wv", "Bits per sample is %d, not 16. Filename='%s'", BitsPerSample, pContextName);
			CallbackData.m_pBuffer = nullptr;
			return false;
		}

		int *pBuffer = (int *)calloc((size_t)NumSamples * NumChannels, sizeof(int));
		if(!WavpackUnpackSamples(pContext, pBuffer, NumSamples))
		{
			free(pBuffer);
			log_error("sound/wv", "WavpackUnpackSamples failed. NumSamples=%d NumChannels=%d Filename='%s'", NumSamples, NumChannels, pContextName);
			CallbackData.m_pBuffer = nullptr;
			return false;
		}

		Sample.m_pData = (short *)calloc((size_t)NumSamples * NumChannels, sizeof(short));

		int *pSrc = pBuffer;
		short *pDst = Sample.m_pData;
		for(int i = 0; i < NumSamples * NumChannels; i++)
			*pDst++ = (short)*pSrc++;

		free(pBuffer);
#ifdef CONF_WAVPACK_CLOSE_FILE
		WavpackCloseFile(pContext);
#endif

		Sample.m_NumFrames = NumSamples;
		Sample.m_Rate = SampleRate;
		Sample.m_Channels = NumChannels;
		Sample.m_LoopStart = -1;
		Sample.m_LoopEnd = -1;
		Sample.m_PausedAt = 0;

		CallbackData.m_pBuffer = nullptr;
	}
	else
	{
		log_error("sound/wv", "Failed to decode sample (%s). Filename='%s'", aError, pContextName);
		CallbackData.m_pBuffer = nullptr;
		return false;
	}

	return true;
}

int main(int argc, char **argv) {
	// Args
	if (argc != 2) {
		printf("usage: %s [music.wav]", argv[0]);
		return 0;
	}

	// Read file
	auto CheckErrno = [](const char *pWhat) {
		if (errno != 0) {
			perror(pWhat);
			exit(1);
		}
	};

	auto File = fopen(argv[1], "rb");
	CheckErrno("fopen %1");
	fseek(File, 0, SEEK_END);
	CheckErrno("fseek SEEK_END");
	auto Size = ftell(File);
	CheckErrno("ftell %1");
	fseek(File, 0, SEEK_SET);
	CheckErrno("fseek SEEK_SET");
	auto Data = malloc(Size);
	fread(Data, 1, Size, File);
	CheckErrno("fread %1");
	fclose(File);

	printf("Read %s (%d bytes)\n", argv[1], (int)Size);

	// Parse
	CSample Sample;
	if (!DecodeWV(Sample, Data, Size, "test")) {
		printf("Decode failed\n");
		return 1;
	}
	printf("Decoded %d frames at %dhz (%f seconds)\n", Sample.m_NumFrames, Sample.m_Rate, (float)Sample.m_NumFrames / (float)Sample.m_Rate);

	free(Data);

	File = fopen("out.wav", "wb");
	CheckErrno("fopen out.wav");

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
