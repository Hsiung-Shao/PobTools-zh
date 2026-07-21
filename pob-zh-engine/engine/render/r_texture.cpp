// SimpleGraphic Engine
// (c) David Gowor, 2014
//
// Module: Render Texture
//

#include <mutex>
#include <vector>
#include <atomic>
#include "r_local.h"

#include "cmp_core.h"
#include "stb_image_resize.h"
#include <gli/gl.hpp>
#include <gli/generate_mipmaps.hpp>

// ===================
// Predefined textures
// ===================

static const byte t_whiteImage[64] = { 
	0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
	0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
	0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
	0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
	0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
	0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
	0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
	0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF
};

static const byte t_blackImage[64 * 4] = {};

static const byte t_defaultTexture[64] = {
	0x7F, 0x7F, 0x7F, 0x7F, 0x7F, 0x7F, 0x7F, 0x7F,
	0x7F, 0x7F, 0x7F, 0x00, 0x00, 0x00, 0x7F, 0x7F,
	0x7F, 0x7F, 0x7F, 0x00, 0x00, 0x00, 0x00, 0x7F,
	0x7F, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x7F,
	0x7F, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x7F,
	0x7F, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x7F,
	0x7F, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x7F,
	0x7F, 0x7F, 0x7F, 0x7F, 0x7F, 0x7F, 0x7F, 0x7F
};

// =======================
// r_ITexManager Interface
// =======================

class t_manager_c: public r_ITexManager, public thread_c {
public:
	// Interface
	int		GetAsyncCount() override;
	void	ProcessPendingTextureUploads() override;

	// Encapsulated
	t_manager_c(r_renderer_c* renderer);
	~t_manager_c();

	r_renderer_c* renderer;

	r_tex_c* whiteTex;
	r_tex_c* blackTex;

	bool	AsyncAdd(r_tex_c* tex);
	bool	AsyncRemove(r_tex_c* tex);

	void	EnqueueTextureUpload(r_tex_c* tex);
	void	RemovePendingTextureUpload(r_tex_c* tex);

private:
	std::atomic<bool> doRun;
	std::atomic<int> runnersRunning;

	std::vector<std::thread> workers;
	std::vector<r_tex_c *> textureQueue;
	std::mutex mutex;

	std::vector<r_tex_c *> uploadQueue;
	std::mutex uploadMutex;

	void	ThreadProc() override;
};

r_ITexManager* r_ITexManager::GetHandle(r_renderer_c* renderer)
{
	return new t_manager_c(renderer);
}

void r_ITexManager::FreeHandle(r_ITexManager* hnd)
{
	delete (t_manager_c*)hnd;
}

t_manager_c::t_manager_c(r_renderer_c* renderer)
	: thread_c(renderer->sys), renderer(renderer)
{
	whiteTex = new r_tex_c(this, "@white", 0);
	blackTex = new r_tex_c(this, "@black", 0);

	doRun = true;
	runnersRunning = 0;
	const int runnersWanted = 4;

	for (int i = 0; i < runnersWanted; ++i)
	{
		workers.emplace_back([this] {
			ThreadProc();
			});
	}
	//ThreadStart();
	while (runnersRunning < runnersWanted) {
		renderer->sys->Sleep( 1 );
	}
}

t_manager_c::~t_manager_c()
{
	doRun = false;
	for (auto& worker : workers)
		worker.join();

	for (auto tex : textureQueue)
		delete tex;

	delete whiteTex;
	delete blackTex;
}

// =====================
// Texture Manager Class
// =====================

int t_manager_c::GetAsyncCount()
{
	std::lock_guard<std::mutex> lock ( mutex );
	return (int)textureQueue.size();
}

void t_manager_c::ProcessPendingTextureUploads()
{
	std::unique_lock lk(uploadMutex);
	for (auto tex : uploadQueue) {
		r_tex_c::PerformUpload(tex);
	}
	uploadQueue.clear();
}

bool t_manager_c::AsyncAdd(r_tex_c* tex)
{
	std::lock_guard<std::mutex> lock( mutex );
	if ( runnersRunning == 0 ) {
		return true;
	}
	textureQueue.push_back( tex );
	tex->status = r_tex_c::IN_QUEUE;
	return false;
}

bool t_manager_c::AsyncRemove(r_tex_c* tex)
{
	{
		std::lock_guard<std::mutex> lock( mutex );
		if (tex->status == r_tex_c::IN_QUEUE) {
			for (auto itr = textureQueue.begin(); itr != textureQueue.end(); ++itr) {
				if (*itr == tex) {
					textureQueue.erase( itr );
					tex->status = r_tex_c::INIT;
					return false;
				}
			}
		}
	}
	while (tex->status == r_tex_c::PROCESSING || tex->status == r_tex_c::SIZE_KNOWN) {
		renderer->sys->Sleep( 1 );
	}

	if (tex->status == r_tex_c::PENDING_UPLOAD) {
		RemovePendingTextureUpload(tex);
	}
	
	return true;
}

void t_manager_c::EnqueueTextureUpload(r_tex_c* tex)
{
	std::scoped_lock lk(uploadMutex);
	uploadQueue.push_back(tex);
}

void t_manager_c::RemovePendingTextureUpload(r_tex_c* tex)
{
	std::scoped_lock lk(uploadMutex);
	if (auto I = std::find(uploadQueue.begin(), uploadQueue.end(), tex); I != uploadQueue.end())
		uploadQueue.erase(I);
}

void t_manager_c::ThreadProc()
{
	++runnersRunning;
	while (doRun) {
		r_tex_c *doTex = nullptr;
		{
			std::lock_guard<std::mutex> lock( mutex );

			// Find a texture with the highest loading priority
			int maxPri = 0;
			auto doTexItr = textureQueue.end();
			for (auto curTexItr = textureQueue.begin(); curTexItr != textureQueue.end(); ++curTexItr) {
				auto curTex = *curTexItr;
				if (doTexItr == textureQueue.end() || curTex->loadPri > maxPri) {
					maxPri = curTex->loadPri;
					doTexItr = curTexItr;
				}
			}

			if (doTexItr != textureQueue.end()) {
				doTex = *doTexItr;
				textureQueue.erase(doTexItr);
				doTex->status = r_tex_c::PROCESSING;
			}
		}
	
		if (doTex != nullptr) {
			// Load this texture
			doTex->LoadFile();
			doTex = nullptr;
		} else {
			// Idle
			renderer->sys->Sleep(1);
		}
	}
	--runnersRunning;
}

// ===============
// Image Resampler
// ===============

class t_sampleDim_c {
public:
	int		max = 0;
	int		i1 = 0, i2 = 0;
	double	w1 = 0.0, w2 = 0.0;

	t_sampleDim_c(int imax)
	{
		max = imax;
	}

	void GenIndicies(double di)
	{
		i1 = (int)floor(di);
		i2 = (int)ceil(di);
		w2 = di - i1;
		w1 = 1.0f - w2;
		if (i2 >= max) {
			i2 = max - 1;
		}
	}
};

static void T_ResampleImage(byte* in, dword in_w, dword in_h, int in_comp, byte* out, dword out_w, dword out_h)
{
	// Initialise sample dimensions
	t_sampleDim_c six(in_w), siy(in_h);

	double xst = (double)in_w / out_w;
	double yst = (double)in_h / out_h;
		
	double dy = 0;
	for (dword y = 0; y < out_h; y++, dy+= yst) {
		// Generate Y indicies
		siy.GenIndicies(dy);
		double dx = 0;
		for (dword x = 0; x < out_w; x++, dx+= xst) {
			// Generate X indicies
			six.GenIndicies(dx);

			// Resample each component
			for (int c = 0; c < in_comp; c++) {
				out[in_comp * (y*out_w + x) + c] = 
					(byte)
					(
						(double)in[in_comp * (siy.i1 * six.max + six.i1) + c] * six.w1 * siy.w1 + 
						(double)in[in_comp * (siy.i2 * six.max + six.i1) + c] * six.w1 * siy.w2 +
						(double)in[in_comp * (siy.i1 * six.max + six.i2) + c] * six.w2 * siy.w1 + 
						(double)in[in_comp * (siy.i2 * six.max + six.i2) + c] * six.w2 * siy.w2 
					);
			}
		}
	}
}

// ====================
// OpenGL Texture Class
// ====================

r_tex_c::r_tex_c(r_ITexManager* manager, std::string_view fileName, int flags)
{
	Init(manager, fileName, flags);

	StartLoad();
	if (status == INIT) {
		// Load it now
		LoadFile();
	}
}

r_tex_c::r_tex_c(r_ITexManager* manager, std::unique_ptr<image_c> i_img, int flags)
{
	Init(manager, {}, flags);

	// Direct upload
	this->img = BuildMipSet(std::move(i_img));
	PerformUpload(this);
}

r_tex_c::~r_tex_c()
{
	if (status >= IN_QUEUE && status < DONE) {
		manager->AsyncRemove(this);
	}
	glDeleteTextures(1, &texId);
}

void r_tex_c::Init(r_ITexManager* i_manager, std::string_view i_fileName, int i_flags)
{
	manager = (t_manager_c*)i_manager;
	renderer = manager->renderer;
	error = 0;
	status = INIT;
	loadPri = 0;
	texId = 0;
	flags = i_flags;
	fileName = i_fileName;
	fileWidth = 0;
	fileHeight = 0;
}

void r_tex_c::Bind()
{
	if (status == DONE) {
		glBindTexture(target, texId);
	} else {
		manager->blackTex->Bind();
	}
}

void r_tex_c::Unbind()
{
	glBindTexture(target, 0);
}

void r_tex_c::Enable()
{	
	glEnable(GL_TEXTURE_2D);
}

void r_tex_c::Disable()
{
	Unbind();
	glDisable(GL_TEXTURE_2D);
}

void r_tex_c::StartLoad()
{
	if (flags & TF_ASYNC)
		manager->AsyncAdd(this);
}

void r_tex_c::AbortLoad()
{
	manager->AsyncRemove(this);
}

void r_tex_c::ForceLoad()
{
	if (status == INIT) {
		LoadFile();
	} else if (fileWidth == 0) {
		// Load not pending, do it now
		LoadFile();
	}
}

std::unique_ptr<image_c> r_tex_c::BuildMipSet(std::unique_ptr<image_c> img)
{
	const auto format = img->tex.format();

	const bool blockCompressed = is_compressed(format);
	const bool isAsync = !!(flags & TF_ASYNC);
	const bool hasExistingMips = img->tex.layers() > 1;

	auto extent = img->tex.extent();
	const auto maxDim = (int)renderer->texMaxDim;
	auto numLevels = img->tex.levels();

	const auto shrinksNeeded = [&t = img->tex, maxDim] {
		auto extent = t.extent();
		int shrinks = 0;
		for (; extent.x > maxDim && extent.y > maxDim; ++shrinks) {
			extent.x /= 2;
			extent.y /= 2;
		}
		return shrinks;
		}();

	// There is an invariant we need to maintain here of that no sides may exceed the maximum dimensions of the renderer.
	// For block-compressed textures we could drop finer mips until we reach a coarser level that's sufficiently reduced in size.
	// We can't generate additional levels for those unless we pull in block format decoding via something like Compressonator
	// and then we would have to consider whether to upload recompressed or burn VRAM on a non-compressed texture.
	// For regular textures we can resize proportionally down for the largest axis to reach the max dimension.

	if (shrinksNeeded) {
		if (shrinksNeeded >= numLevels) {
			// Not enough levels in texture to satsify shrinking requirement.
			if (blockCompressed) {
				// TODO(zao): Fail hard, ignore, or decompress+rescale.
			}
			else {
				// TODO(zao): Synthesise a new top level for all layers.
				auto smallExtent = img->tex.extent(img->tex.levels() - 1);
			}
		}
		else {
			auto& t = img->tex;
			t = gli::texture2d_array(t,
				t.base_layer(), t.max_layer(),
				t.base_level() + shrinksNeeded, t.max_level());
		}
	}

	const bool generateMips = !blockCompressed && img->tex.levels() == 1 && !(flags & TF_NOMIPMAP);

	if (generateMips) {
		if (blockCompressed) {
			// TODO(LV): Mipmap generation requested for a block-compressed texture. This requires decompression.
		}
		else {
			const auto format = img->tex.format();
			const auto extent = img->tex.extent();
			const auto layers = img->tex.layers();
			const auto swizzles = img->tex.swizzles();
			auto newTex = gli::texture2d_array(format, extent, layers, swizzles);
			for (size_t layer = 0; layer < layers; ++layer) {
				newTex.copy(img->tex, layer, 0, 0, layer, 0, 0);
				const size_t levels = newTex.levels();
				for (size_t level = 1; level < levels; ++level) {
					const auto srcExtent = newTex.extent(level - 1);
					const auto comp = (int)gli::component_count(format);
					const auto dstExtent = newTex.extent(level);
					const bool hasAlpha = comp == 4;
					stbir_resize_uint8_srgb_edgemode(
						newTex.data<uint8_t>(layer, 0, level - 1), srcExtent.x, srcExtent.y, srcExtent.x * comp,
						newTex.data<uint8_t>(layer, 0, level), dstExtent.x, dstExtent.y, dstExtent.x * comp,
						comp, hasAlpha ? 3 : STBIR_ALPHA_CHANNEL_NONE, 0, STBIR_EDGE_CLAMP);
				}
			}
			//newTex = gli::generate_mipmaps(newTex, gli::FILTER_LINEAR);
			img->tex = newTex;
		}
	}
	return img;
}

static gli::texture2d_array TranscodeTexture(gli::texture2d_array src, gli::format dstFormat, bool dropFinestMipIfPossible)
{
	// Very limited format support, only really sufficient as a fallback when BC7 isn't available.

	// Source formats: BC7
	const auto srcFormat = src.format();
	if (src.format() != gli::FORMAT_RGBA_BP_UNORM_BLOCK16)
		return src;

	// Destination formats: BC3 or RGBA8
	if (dstFormat != gli::FORMAT_RGBA_DXT5_UNORM_BLOCK16 && dstFormat != gli::FORMAT_RGBA8_UNORM_PACK8)
		return src;

	// To save VRAM and processing costs, there is the option to discard the finest mip level of the source if there's coarser levels available.
	// If so, the transcoding will generate destination levels 0..n-1 from levels 1..n of the source.
	size_t firstLevel = 0;
	if (dropFinestMipIfPossible && src.levels() > 1)
		firstLevel = 1;

	const auto outExtent = src.extent(firstLevel);
	const auto outLayers = src.layers();
	const auto outLevels = src.levels() - firstLevel;

	gli::texture2d_array dst(dstFormat, outExtent, outLayers, outLevels);

	std::array<uint8_t, 64> rgba{};
	for (size_t layer = 0; layer < outLayers; ++layer) {
		for (size_t dstLevel = 0; dstLevel < outLevels; ++dstLevel) {
			auto* dstData = (uint8_t*)dst.data(layer, 0, dstLevel);
			const auto dstExtent = dst.extent(dstLevel);
			const auto dstRowStride = dstExtent.x * 4;

			const size_t srcLevel = dstLevel + firstLevel;
			const auto* srcData = (const uint8_t*)src.data(layer, 0, srcLevel);

			const auto srcBlockSize = gli::block_extent(srcFormat);
			const auto srcBlocksPerRow = (dstExtent.y + srcBlockSize.y - 1) / srcBlockSize.y; // round up partial blocks
			const auto srcBlocksPerColumn = (dstExtent.x + srcBlockSize.x - 1) / srcBlockSize.x; // -''-

			for (size_t blockRow = 0; blockRow < srcBlocksPerRow; ++blockRow) {
				const size_t rowBase = blockRow * srcBlockSize.y;
				const size_t rowsLeft = (std::min)(4ull, dstExtent.y - rowBase);

				for (size_t blockCol = 0; blockCol < srcBlocksPerColumn; ++blockCol) {
					// Read source 4x4 texel block, no branching needed.
					DecompressBlockBC7(srcData, rgba.data());

					// Recompress or distribute the 4x4 RGBA block.
					if (dstFormat == gli::FORMAT_RGBA_DXT5_UNORM_BLOCK16) {
						// The block order in the level data for BC3 is the same as for BC7, so we can just append them as they appear.
						CompressBlockBC3(rgba.data(), 16, dstData + blockCol * gli::block_size(dstFormat));

						// Advance the storage write pointer as we go.
						dstData += gli::block_size(dstFormat);
					}
					else if (dstFormat == gli::FORMAT_RGBA8_UNORM_PACK8) {
						// Compressed blocks unconditionally have 4x4 texels each, even if the source extent isn't evenly divisible into blocks with padding on the right and bottom of the block.
						// When copying these to RGBA storage which doesn't have this padding we need to ensure we don't go past the edges of the destination.

						// Here we work off that dstData points at the top left pixel of the block row in the destination.
						const size_t colBase = blockCol * srcBlockSize.x;
						const size_t colsLeft = (std::min)(4ull, dstExtent.x - colBase);
						const size_t colBytesLeft = colsLeft * 4;
						for (size_t innerRow = 0; innerRow < rowsLeft; ++innerRow) {
							auto* dstPtr = dstData + dstRowStride * innerRow + colBase * 4;
							memcpy(dstPtr, rgba.data() + innerRow * 16, colBytesLeft);
						}
						// Note that dstData is advanced at the end of the source block row to make copy logic easier to follow.
					}
					srcData += gli::block_size(srcFormat);
				}

				// Advance the destination buffer only at the end of an source block row if writing to RGBA output.
				if (!gli::is_compressed(dstFormat))
					dstData += dstRowStride * rowsLeft;
			}

			const auto* srcEnd = srcData + src.size(srcLevel);
			const auto* dstEnd = dstData + dst.size(dstLevel);
			assert(srcData == srcEnd);
			assert(dstData == dstEnd);
		}
	}

	return dst;
}

void r_tex_c::LoadFile()
{
	if (_stricmp(fileName.c_str(), "@white") == 0) {
		// Upload an 8x8 white image
		auto raw = std::make_unique<image_c>();
		raw->CopyRaw(IMGTYPE_GRAY, 8, 8, t_whiteImage);
		Upload(*raw, TF_NOMIPMAP);
		status = DONE;
		return;
	}
	else if (_stricmp(fileName.c_str(), "@black") == 0) {
		// Upload an 8x8 black image
		auto raw = std::make_unique<image_c>();
		raw->CopyRaw(IMGTYPE_RGBA, 8, 8, t_blackImage);
		Upload(*raw, TF_NOMIPMAP);
		status = DONE;
		return;
	}

	// Try to load image file using appropriate loader
	auto path = std::filesystem::u8path(fileName);
	img = std::unique_ptr<image_c>(image_c::LoaderForFile(renderer->sys->con, path));
	if (img) {
		auto sizeCallback = [this](int width, int height) {
			this->fileWidth = width;
			this->fileHeight = height;
			this->status = SIZE_KNOWN;
		};
		error = img->Load(path, sizeCallback);
		if ( !error ) {
			const bool useTextureFormatFallback = !renderer->texBC7;
			if (useTextureFormatFallback) {
				if (img->tex.format() == gli::FORMAT_RGBA_BP_UNORM_BLOCK16)
					img->tex = TranscodeTexture(img->tex, gli::FORMAT_RGBA8_UNORM_PACK8, true);
			}
			stackLayers = img->tex.layers();
			const bool is_async = !!(flags & TF_ASYNC);
			img = BuildMipSet(std::move(img));

			status = PENDING_UPLOAD;
			if (is_async) {
				// Post a main thread task to create and fill GPU textures.
				manager->EnqueueTextureUpload(this);
			}
			else {
				PerformUpload(this);
			}
			return;
		}
	}

	auto raw = std::make_unique<image_c>();
	raw->CopyRaw(IMGTYPE_GRAY, 8, 8, t_defaultTexture);
	Upload(*raw, TF_NOMIPMAP);
	status = DONE;
}

void r_tex_c::PerformUpload(r_tex_c* tex)
{
	tex->Upload(*tex->img, tex->flags);
	tex->img = {};
	tex->status = DONE;
}

static std::atomic<size_t> inputBytes = 0;
static std::atomic<size_t> uploadedBytes = 0;

void r_tex_c::Upload(image_c& img, int flags)
{
	static gli::gl gl(gli::gl::PROFILE_ES30);

	const auto& tex = img.tex;
	target = gl.translate(tex.target());
	const auto format = gl.translate(tex.format(), tex.swizzles());

	// Find and bind texture name
	glGenTextures(1, &texId);
	glBindTexture(target, texId);

	glPixelStorei(GL_UNPACK_ALIGNMENT, 1);

	glTexParameteri(target, GL_TEXTURE_BASE_LEVEL, 0);
	glTexParameteri(target, GL_TEXTURE_MAX_LEVEL, (GLint)tex.levels());
	glTexParameteri(target, GL_TEXTURE_SWIZZLE_R, format.Swizzles.r);
	glTexParameteri(target, GL_TEXTURE_SWIZZLE_G, format.Swizzles.g);
	glTexParameteri(target, GL_TEXTURE_SWIZZLE_B, format.Swizzles.b);
	glTexParameteri(target, GL_TEXTURE_SWIZZLE_A, format.Swizzles.a);

	const int miplevels = (int)tex.levels();

	// Set filters
	if (miplevels == 1) {
		glTexParameteri(target, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
	}
	else {
		glTexParameteri(target, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR);
	}
	if (flags & TF_NEAREST) {
		glTexParameteri(target, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
	}
	else {
		glTexParameteri(target, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
	}

	constexpr float anisotropyCap = 16.0f;
	static const float maxAnisotropy = [] {
		float ret{};
		glGetFloatv(GL_MAX_TEXTURE_MAX_ANISOTROPY_EXT, &ret);
		return ret;
		}();
	glTexParameterf(target, GL_TEXTURE_MAX_ANISOTROPY, (std::min)(maxAnisotropy, anisotropyCap));

	// Set repeating
	if (flags & TF_CLAMP) {
		glTexParameteri(target, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
		glTexParameteri(target, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
	}
	else {
		glTexParameteri(target, GL_TEXTURE_WRAP_S, GL_REPEAT);
		glTexParameteri(target, GL_TEXTURE_WRAP_T, GL_REPEAT);
	}

	const int layers = (int)tex.layers();
	const auto extent = tex.extent();
	const bool isTextureArray = target == GL_TEXTURE_2D_ARRAY;

	if (isTextureArray)
		glTexStorage3D(target, miplevels, format.Internal, extent.x, extent.y, layers);
	else
		glTexStorage2D(target, miplevels, format.Internal, extent.x, extent.y);

	for (int layer = 0; layer < layers; ++layer) {
		for (int miplevel = 0; miplevel < miplevels; ++miplevel) {

			const auto extent = tex.extent(miplevel);

			const int up_w = extent.x;
			const int up_h = extent.y;

			// Upload the mipmap
			uploadedBytes += tex.size(miplevel);
			const auto* data = tex.data(layer, 0, miplevel);
			if (is_compressed(tex.format()))
				if (isTextureArray)
					glCompressedTexSubImage3D(target, miplevel, 0, 0, layer, extent.x, extent.y, 1, format.Internal, (GLsizei)tex.size(miplevel), data);
				else
					glCompressedTexSubImage2D(target, miplevel, 0, 0, extent.x, extent.y, format.Internal, (GLsizei)tex.size(miplevel), data);
			else
				if (isTextureArray)
					glTexSubImage3D(target, miplevel, 0, 0, layer, extent.x, extent.y, 1, format.External, format.Type, data);
				else
					glTexSubImage2D(target, miplevel, 0, 0, extent.x, extent.y, format.External, format.Type, data);
		}
	}
}
