#include "core_compress.h"

std::optional<std::vector<char>> CompressZstandard(gsl::span<const std::byte> src, std::optional<int> level)
{
	if (!level)
		level = ZSTD_defaultCLevel();
	std::vector<char> dst(ZSTD_compressBound(src.size()));
	size_t rc = ZSTD_compress(dst.data(), dst.size(), src.data(), src.size(), *level);
	if (ZSTD_isError(rc))
		return {};
	dst.resize(rc);
	return dst;
}

std::optional<std::vector<char>> DecompressZstandard(gsl::span<const std::byte> src)
{
	const size_t buffOutSize = ZSTD_DStreamOutSize();
	std::vector<char> buffOut(buffOutSize);

	std::vector<char> dst;
	dst.reserve(1 << 20);

	std::shared_ptr<ZSTD_DCtx> dctx(ZSTD_createDCtx(), &ZSTD_freeDCtx);
	ZSTD_inBuffer input = { src.data(), src.size(), 0 };
	while (input.pos < input.size) {
		ZSTD_outBuffer output = { buffOut.data(), buffOut.size(), 0 };
		const size_t rc = ZSTD_decompressStream(dctx.get(), &output, &input);
		if (ZSTD_isError(rc)) {
			return {};
		}
		auto oldSize = dst.size();
		auto newSize = oldSize + output.pos;
		if (newSize > dst.capacity()) {
			dst.reserve((size_t)(newSize * 1.5));
		}
		dst.resize(newSize);
		memcpy(dst.data() + oldSize, output.dst, output.pos);
	}

	dst.shrink_to_fit();
	return dst;
}
