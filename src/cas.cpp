#include <Windows.h>
#include <intsafe.h>
#include <dxgi.h>
#include <d3d11_1.h>

#include <algorithm>
#include <atomic>

#include <wil/com.h>

#define A_CPU 1
#include "ffx_a.h"
#include "ffx_cas.h"


// グローバル変数
HINSTANCE g_dll_handle;


// VLC Mediaplayerで扱う設定項目の名称
#define OPTION_KEY_PREFIX "cas-"
#define OPTION_KEY_ADAPTER "adapter"
#define OPTION_KEY_SHARPNESS "sharpness"
#define OPTION_KEY_FP16PREFER "fp16prefer"
static const char *const kFilterOptions[] =
{
	OPTION_KEY_ADAPTER,
	OPTION_KEY_SHARPNESS,
	OPTION_KEY_FP16PREFER,
	nullptr
};
static const char *kVarNameAdapter = OPTION_KEY_PREFIX OPTION_KEY_ADAPTER;
static const char *kVarNameSharpness = OPTION_KEY_PREFIX OPTION_KEY_SHARPNESS;
static const char *kVarNameFp16prefer = OPTION_KEY_PREFIX OPTION_KEY_FP16PREFER;

// 定数バッファのサイズ
static const UINT kArgumentBufferSize = 32;

// VLCの各種ヘッダ内で使用するため、定義しておく
typedef SSIZE_T ssize_t;

// vlc_threads.h で参照するため、定義しておく
int poll(struct pollfd *fds, unsigned nfds, int timeout)
{
	RaiseException(STATUS_NONCONTINUABLE_EXCEPTION, EXCEPTION_SOFTWARE_ORIGINATE, 0, nullptr);
	return 0;
}

// vlc_plugin.h で参照するため、事前に定義しておく
#define MODULE_STRING ("libcas_plugin")

#include <vlc_common.h>
#include <vlc_plugin.h>
#include <vlc_filter.h>
#include <vlc_picture.h>
#include <vlc_variables.h>


struct filter_sys_t
{
	ID3D11Device *device_;
	ID3D11DeviceContext *device_context_;
	ID3D11Texture2D *dynamic_texture_; // シェーダ入力、ここにピクチャをコピーする
	ID3D11Texture2D *default_texture_; // シェーダ出力
	ID3D11Texture2D *staging_texture_; // シェーダ出力をこれにコピーしてからCPUで読み取り、出力ピクチャにコピーする
	ID3D11Buffer *argumanet_buffer_; // シェーダ入力、幅、高さ、シャープネス値を与える
	ID3D11UnorderedAccessView *uav_; // CSSetUnorderedAccessViewsでUAVの保持を行う旨の記述が無いので、一応保存しておく
	ID3D11ComputeShader *cas_shader_;
	float width_;
	float height_;
	std::atomic<float> sharpness_;
};


// VLC Mediaplayerからのコールバック関数
int Open(vlc_object_t *obj);
void Close(vlc_object_t *obj);
picture_t *Filter(filter_t *filter, picture_t *input_picture);
int VariableChangeCallback(vlc_object_t *obj, char const *variable_name, vlc_value_t old_value, vlc_value_t new_value, void *data);

// 非コールバック関数
void VlcLog(vlc_object_t *obj, vlc_log_type prio, const char *format, ...);
bool SetupCom();
bool CreateComputeShader(ID3D11ComputeShader **shader, ID3D11Device *device, HMODULE module, const char *resource_type, const char *resource_name);
bool ValidatePicture(filter_t *filter, picture_t *input_picture);
bool CopyPictureToDynamicTexture(filter_t *filter, picture_t *input_picture);
void Cas(filter_t *filter, picture_t *input_picture);
void CopyDefaultTextureToStagingTexture(filter_t *filter);
bool CopyStagingTextureToPicture(filter_t *filter, picture_t *output_picture);

// DLL エントリポイント
// DLL内のリソースを読むために、DLLのハンドルをグローバル変数に保存する
BOOL WINAPI DllMain(HINSTANCE hinstDLL, DWORD fdwReason, LPVOID lpvReserved)
{
	UNREFERENCED_PARAMETER(lpvReserved);

	switch (fdwReason)
	{
	case DLL_PROCESS_ATTACH:
		DisableThreadLibraryCalls(hinstDLL);
		g_dll_handle = hinstDLL;
		break;

	case DLL_PROCESS_DETACH:
		g_dll_handle = nullptr;
		break;
	}

	return TRUE;
}

int Open(vlc_object_t *obj)
{
	filter_t *filter = reinterpret_cast<filter_t *>(obj);
	wil::com_ptr<IDXGIFactory1> dxgi_factory;
	wil::com_ptr<IDXGIAdapter1> adapter;
	wil::com_ptr_nothrow<ID3D11Device> device;
	wil::com_ptr<ID3D11DeviceContext> device_context;
	wil::com_ptr<ID3D11Texture2D> dynamic_texture;
	wil::com_ptr<ID3D11Texture2D> default_texture;
	wil::com_ptr<ID3D11Texture2D> staging_texture;
	wil::com_ptr<ID3D11ComputeShader> cas_shader;
	wil::com_ptr<ID3D11Buffer> argumanet_buffer;
	wil::com_ptr<ID3D11ShaderResourceView> srv;
	wil::com_ptr<ID3D11UnorderedAccessView> uav;


	if (!SetupCom())
	{
		VlcLog(obj, VLC_MSG_ERR, "Failed COM setup.");
		return VLC_EGENERIC;
	}

	// chroma format priority (auto)
	// VLC_CODEC_D3D11_OPAQUE
	// VLC_CODEC_I420
	// VLC_CODEC_I422
	// VLC_CODEC_I420_10L
	// VLC_CODEC_I420_10B
	// VLC_CODEC_I420_16L
	// VLC_CODEC_RGB32		DXGI_FORMAT_B8G8R8A8_UNORM
	// VLC_CODEC_RGB24
	// VLC_CODEC_RGBA		DXGI_FORMAT_B8G8R8A8_UNORM
	if (VLC_CODEC_RGB32 != filter->fmt_in.video.i_chroma)
	{
		VlcLog(obj, VLC_MSG_ERR, "Input video format is not VLC_CODEC_RGB32");
		return VLC_EGENERIC;
	}

	if (!video_format_IsSimilar(&filter->fmt_in.video, &filter->fmt_out.video))
	{
		VlcLog(obj, VLC_MSG_ERR, "Input and output formats are different.");
		return VLC_EGENERIC;
	}

	if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&dxgi_factory))))
	{
		VlcLog(obj, VLC_MSG_ERR, "Failed CreateDXGIFactory1");
		return VLC_EGENERIC;
	}

	// 設定項目を利用するための準備
	config_ChainParse(obj, OPTION_KEY_PREFIX, kFilterOptions, filter->p_cfg);

	UINT adapter_index = static_cast<UINT>(var_GetInteger(obj, kVarNameAdapter));

	dxgi_factory->EnumAdapters1(adapter_index, &adapter);

	D3D_FEATURE_LEVEL d3d_feature_levels[] = {D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0};

	// 指定したアダプタのオブジェクトを取得できた場合
	if (adapter)
	{
		DXGI_ADAPTER_DESC1 desc1{};

		if (FAILED(adapter->GetDesc1(&desc1)))
		{
			VlcLog(obj, VLC_MSG_ERR, "Failed GetDesc1");
			return VLC_EGENERIC;
		}

		// 指定したアダプタが非ソフトウェアアダプタの場合のデバイスオブジェクト生成
		if (!(DXGI_ADAPTER_FLAG_SOFTWARE & desc1.Flags))
		{
			if (FAILED(D3D11CreateDevice(
				adapter.get(),
				D3D_DRIVER_TYPE_UNKNOWN,
				nullptr,
				D3D11_CREATE_DEVICE_BGRA_SUPPORT| D3D11_CREATE_DEVICE_SINGLETHREADED,
				d3d_feature_levels, ARRAYSIZE(d3d_feature_levels),
				D3D11_SDK_VERSION,
				&device,
				nullptr,
				nullptr)))
			{
				VlcLog(obj, VLC_MSG_ERR, "Failed D3D11CreateDevice (valid adapter)");
				return VLC_EGENERIC;
			}

			VlcLog(obj, VLC_MSG_INFO, "D3D11CreateDevice (valid adapter)");
		}
		else
		// 指定したアダプタがソフトウェアアダプタの場合、WARPドライバのデバイスオブジェクトを生成する
		{
			if (FAILED(D3D11CreateDevice(
				nullptr,
				D3D_DRIVER_TYPE_WARP,
				nullptr,
				D3D11_CREATE_DEVICE_BGRA_SUPPORT| D3D11_CREATE_DEVICE_SINGLETHREADED,
				d3d_feature_levels, ARRAYSIZE(d3d_feature_levels),
				D3D11_SDK_VERSION,
				&device,
				nullptr,
				nullptr)))
			{
				VlcLog(obj, VLC_MSG_ERR, "Failed D3D11CreateDevice (WARP adapter)");
				return VLC_EGENERIC;
			}

			VlcLog(obj, VLC_MSG_INFO, "D3D11CreateDevice (WARP adapter)");
		}
	}
	else
	// 指定したアダプタのオブジェクトを取得できなかった場合、ハードウェアドライバタイプのデフォルトのデバイスオブジェクトを生成する
	{
		if (FAILED(D3D11CreateDevice(
			nullptr,
			D3D_DRIVER_TYPE_HARDWARE,
			nullptr,
			D3D11_CREATE_DEVICE_BGRA_SUPPORT| D3D11_CREATE_DEVICE_SINGLETHREADED,
			d3d_feature_levels, ARRAYSIZE(d3d_feature_levels),
			D3D11_SDK_VERSION,
			&device,
			nullptr,
			nullptr)))
		{
			VlcLog(obj, VLC_MSG_ERR, "Failed D3D11CreateDevice (default adapter)");
			return VLC_EGENERIC;
		}

		VlcLog(obj, VLC_MSG_INFO, "D3D11CreateDevice (default adapter)");
	}


	D3D11_TEXTURE2D_DESC texture_desc{};
	texture_desc.Width = filter->fmt_in.video.i_width;
	texture_desc.Height = filter->fmt_in.video.i_height;
	texture_desc.MipLevels = 1;
	texture_desc.ArraySize = 1;
	texture_desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
	texture_desc.SampleDesc.Count = 1;
	texture_desc.SampleDesc.Quality = 0;
	texture_desc.Usage = D3D11_USAGE_DYNAMIC;
	texture_desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
	texture_desc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
	texture_desc.MiscFlags = 0;
	if (FAILED(device->CreateTexture2D(&texture_desc, nullptr, &dynamic_texture)))
	{
		VlcLog(obj, VLC_MSG_ERR, "Failed CreateTexture2D (dynamic texture)");
		return VLC_EGENERIC;
	}

	texture_desc.Usage = D3D11_USAGE_DEFAULT;
	texture_desc.BindFlags = D3D11_BIND_UNORDERED_ACCESS;
	texture_desc.CPUAccessFlags = 0;
	if (FAILED(device->CreateTexture2D(&texture_desc, nullptr, &default_texture)))
	{
		VlcLog(obj, VLC_MSG_ERR, "Failed CreateTexture2D (default texture)");
		return VLC_EGENERIC;
	}

	texture_desc.Usage = D3D11_USAGE_STAGING;
	texture_desc.BindFlags = 0;
	texture_desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
	texture_desc.MiscFlags = 0;
	if (FAILED(device->CreateTexture2D(&texture_desc, nullptr, &staging_texture)))
	{
		VlcLog(obj, VLC_MSG_ERR, "Failed CreateTexture2D (staging texture)");
		return VLC_EGENERIC;
	}

	// シェーダオブジェクトを生成する
	// とりあえずFP32版を生成し、FP16版を優先する指定があればFP16版に交換する
	CreateComputeShader(&cas_shader, device.get(), g_dll_handle, "SHADER", "CAS32");

	if (var_GetBool(obj, kVarNameFp16prefer) || !cas_shader)
	{
		wil::com_ptr<ID3D11Device1> device1;

		// D3D 11.1 未満の場合、min16floatなどを含むシェーダを利用できない
		// シェーダの最小精度を調べるためには、ID3D11Device1 以降が必要
		if (SUCCEEDED(device.query_to<ID3D11Device1>(&device1)))
		{
			D3D11_FEATURE_DATA_SHADER_MIN_PRECISION_SUPPORT min_precision;

			if (SUCCEEDED(device1->CheckFeatureSupport(D3D11_FEATURE_SHADER_MIN_PRECISION_SUPPORT, &min_precision, sizeof (D3D11_FEATURE_DATA_SHADER_MIN_PRECISION_SUPPORT)))
				&& (D3D11_SHADER_MIN_PRECISION_16_BIT & min_precision.AllOtherShaderStagesMinPrecision))
			{
				wil::com_ptr<ID3D11ComputeShader> cas16_shader;

				if (CreateComputeShader(&cas16_shader, device1.get(), g_dll_handle, "SHADER", "CAS16"))
					cas_shader = cas16_shader;
			}
		}
	}

	if (!cas_shader)
	{
		VlcLog(obj, VLC_MSG_ERR, "Failed CreateComputeShader (CAS32 and CAS16)");
		return VLC_EGENERIC;
	}

	//定数バッファの生成
	{
		D3D11_BUFFER_DESC desc{};
		desc.ByteWidth = kArgumentBufferSize;
		desc.Usage = D3D11_USAGE_DEFAULT;
		desc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
		desc.CPUAccessFlags = 0;
		desc.MiscFlags = 0;
		desc.StructureByteStride = 0;

		if (FAILED(device->CreateBuffer(&desc, nullptr, &argumanet_buffer)))
		{
			VlcLog(obj, VLC_MSG_ERR, "Failed CreateBuffer (argument buffer)");
			return VLC_EGENERIC;
		}
	}

	//SRV(dynamic texture)の生成
	{
		D3D11_SHADER_RESOURCE_VIEW_DESC desc{};
		desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
		desc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
		desc.Texture2D.MostDetailedMip = 0;
		desc.Texture2D.MipLevels = 1;

		if (FAILED(device->CreateShaderResourceView(dynamic_texture.get(), &desc, &srv)))
		{
			VlcLog(obj, VLC_MSG_ERR, "Failed CreateShaderResourceView");
			return VLC_EGENERIC;
		}
	}

	//UAV(default texture)の生成
	{
		D3D11_UNORDERED_ACCESS_VIEW_DESC desc{};
		desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
		desc.ViewDimension = D3D11_UAV_DIMENSION_TEXTURE2D;
		desc.Texture2D.MipSlice = 0;

		if (FAILED(device->CreateUnorderedAccessView(default_texture.get(), &desc, &uav)))
		{
			VlcLog(obj, VLC_MSG_ERR, "Failed CreateUnorderedAccessView");
			return VLC_EGENERIC;
		}
	}

	device->GetImmediateContext(&device_context);

	//CS, CB, SRV, UAVの設定
	{
		device_context->CSSetShader(cas_shader.get(), nullptr, 0);

		ID3D11Buffer *constant_buffers[] = {argumanet_buffer.get()};
		device_context->CSSetConstantBuffers(0, 1, constant_buffers);

		ID3D11ShaderResourceView *srvs[] = {srv.get()};
		device_context->CSSetShaderResources(0, 1, srvs);

		ID3D11UnorderedAccessView *uavs[] = {uav.get()};
		device_context->CSSetUnorderedAccessViews(0, 1, uavs, nullptr);
	}

	float sharpness = var_GetFloat(obj, kVarNameSharpness);
	sharpness = std::clamp(sharpness, 0.0f, 1.0f);

	// 呼出側への書込
	filter->p_sys = new(std::nothrow) filter_sys_t;
	if (!filter->p_sys)
	{
		VlcLog(obj, VLC_MSG_ERR, "Can not allocate filter_sys_t");
		return VLC_ENOMEM;
	}

	filter->p_sys->device_ = device.detach();
	filter->p_sys->device_context_ = device_context.detach();
	filter->p_sys->dynamic_texture_ = dynamic_texture.detach();
	filter->p_sys->default_texture_ = default_texture.detach();
	filter->p_sys->staging_texture_ = staging_texture.detach();
	filter->p_sys->cas_shader_ = cas_shader.detach();
	filter->p_sys->argumanet_buffer_ = argumanet_buffer.detach();
	filter->p_sys->uav_ = uav.detach();
	filter->p_sys->width_ = static_cast<AF1>(filter->fmt_in.video.i_width);
	filter->p_sys->height_ = static_cast<AF1>(filter->fmt_in.video.i_height);
	filter->p_sys->sharpness_ = sharpness;

	filter->pf_video_filter = Filter;

	var_AddCallback(obj, kVarNameSharpness, VariableChangeCallback, nullptr);

	VlcLog(obj, VLC_MSG_INFO, "Open success");

	return VLC_SUCCESS;
}

void Close(vlc_object_t *obj)
{
	filter_t *filter = reinterpret_cast<filter_t *>(obj);

	filter->p_sys->device_->Release();
	filter->p_sys->device_context_->Release();
	filter->p_sys->dynamic_texture_->Release();
	filter->p_sys->default_texture_->Release();
	filter->p_sys->staging_texture_->Release();
	filter->p_sys->cas_shader_->Release();
	filter->p_sys->argumanet_buffer_->Release();
	filter->p_sys->uav_->Release();

	var_DelCallback(obj, kVarNameSharpness, VariableChangeCallback, nullptr);

	VlcLog(obj, VLC_MSG_INFO, "Close success");

	delete filter->p_sys;
}

picture_t *Filter(filter_t *filter, picture_t *input_picture)
{
	vlc_object_t *obj = VLC_OBJECT(filter);
	picture_t *output_picture;


	// 入力ピクチャが無い場合
	if (!input_picture)
	{
		VlcLog(obj, VLC_MSG_INFO, "Null input picture.");
		return nullptr;
	}

	// 出力ピクチャの割り当てを行う
	output_picture = filter_NewPicture(filter);
	if (!output_picture)
	{
		VlcLog(obj, VLC_MSG_INFO, "Can not prepare new picture.");
		picture_Release(input_picture);
		return nullptr;
	}

	// 入力ピクチャのフォーマットかサイズが不正な場合、出力ピクチャに入力ピクチャをコピーして返す
	if (!ValidatePicture(filter, input_picture))
	{
		VlcLog(obj, VLC_MSG_INFO, "Invalid picture.");
		picture_Copy(output_picture, input_picture);
		picture_Release(input_picture);
		return output_picture;
	}

	// pictureの内容をdynamic textureへコピーすることを試みる
	// 失敗した場合、出力ピクチャに入力ピクチャをコピーし返す
	if (!CopyPictureToDynamicTexture(filter, input_picture))
	{
		VlcLog(obj, VLC_MSG_INFO, "Failed CopyPictureToDynamicTexture");
		picture_Copy(output_picture, input_picture);
		picture_Release(input_picture);
		return output_picture;
	}

	// dynamic textureを読み、CASの処理結果をdefault textureに書く
	Cas(filter, input_picture);

	// default textureの内容をstaging textureへコピーする
	CopyDefaultTextureToStagingTexture(filter);

	// staging textureの内容をoutput_pictureへコピーする
	// 失敗した場合、出力ピクチャに入力ピクチャをコピーし返す
	if (!CopyStagingTextureToPicture(filter, output_picture))
	{
		VlcLog(obj, VLC_MSG_INFO, "Failed CopyStagingTextureToPicture");
		picture_Copy(output_picture, input_picture);
		picture_Release(input_picture);
		return output_picture;
	}

	picture_CopyProperties(output_picture, input_picture);
	picture_Release(input_picture);

	return output_picture;
}

int VariableChangeCallback(vlc_object_t *obj, char const *variable_name, vlc_value_t old_value, vlc_value_t new_value, void *data)
{
	filter_t *filter = reinterpret_cast<filter_t *>(obj);
	filter_sys_t *sys = reinterpret_cast<filter_sys_t *>(filter->p_sys);

	if (0 == strcmp(kVarNameSharpness, variable_name))
		sys->sharpness_.store(new_value.f_float);

	return VLC_SUCCESS;
}

void VlcLog(vlc_object_t *obj, vlc_log_type prio, const char *format, ...)
{
	va_list ap;

	va_start(ap, format);
	vlc_vaLog(obj, prio, vlc_module_name, nullptr, 0, nullptr, format, ap);
	va_end(ap);
}

bool SetupCom()
{
	HRESULT result;
	APTTYPE apt;
	APTTYPEQUALIFIER apt_qualifier;

	result = CoGetApartmentType(&apt, &apt_qualifier);
	switch (result)
	{
	case CO_E_NOTINITIALIZED:
		if (FAILED(CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED)))
			return false;
		else
			return true;

	case S_OK:
		return true;
	}

	return false;
}

bool CreateComputeShader(ID3D11ComputeShader **shader, ID3D11Device *device, HMODULE module, const char *resource_type, const char *resource_name)
{
	if (!shader || !device || !resource_type || !resource_name)
		false;

	HRSRC resource_handle = FindResourceA(module, resource_name, resource_type);
	if (!resource_handle)
		return false;

	HGLOBAL data_handle = LoadResource(module, resource_handle);
	if (!data_handle)
		return false;

	void *bytecode = LockResource(data_handle);
	if (!bytecode)
		return false;

	DWORD bytecode_size = SizeofResource(module, resource_handle);
	if (!bytecode_size)
		return false;

	if (FAILED(device->CreateComputeShader(bytecode, bytecode_size, nullptr, shader)))
		return false;

	return true;
}

bool ValidatePicture(filter_t *filter, picture_t *input_picture)
{
	video_format_t *format = &input_picture->format;
	D3D11_TEXTURE2D_DESC desc;

	filter->p_sys->dynamic_texture_->GetDesc(&desc);

	if (VLC_CODEC_RGB32 != format->i_chroma)
		return false;

	if (desc.Width < format->i_visible_width)
		return false;

	if (desc.Height < format->i_visible_height)
		return false;

	return true;
}

bool CopyPictureToDynamicTexture(filter_t *filter, picture_t *input_picture)
{
	ID3D11DeviceContext *device_context = filter->p_sys->device_context_;
	ID3D11Texture2D *dynamic_texture = filter->p_sys->dynamic_texture_;
	D3D11_MAPPED_SUBRESOURCE mapped;
	plane_t *plane = &input_picture->p[0];

	if (FAILED(device_context->Map(dynamic_texture, 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped)))
		return false;

	uint8_t *dst = reinterpret_cast<uint8_t *>(mapped.pData);
	uint8_t *src = plane->p_pixels;
	for (int y=0; y<plane->i_visible_lines; ++y)
	{
		CopyMemory(dst, src, plane->i_visible_pitch); // おそらく、CopyMemoryを呼ぶよりも単純なループでコピーする方が速い
		dst += mapped.RowPitch;
		src += plane->i_pitch;
	}

	device_context->Unmap(dynamic_texture, 0);

	return true;
}

void Cas(filter_t *filter, picture_t *input_picture)
{
	ID3D11DeviceContext *device_context = filter->p_sys->device_context_;
	ID3D11Buffer *argument_buffer = filter->p_sys->argumanet_buffer_;
	AF1 width = filter->p_sys->width_;
	AF1 height = filter->p_sys->height_;
	AF1 sharpness = filter->p_sys->sharpness_.load();
	varAU4(const0);
	varAU4(const1);
	AU1 constants[8];

	// シェーダの引数を定数バッファに書き込む
	CasSetup(const0, const1, sharpness, width, height, width, height);
	CopyMemory(constants, const0, sizeof (const0));
	CopyMemory(constants+4, const1, sizeof (const1));
	device_context->UpdateSubresource(argument_buffer, 0, nullptr, constants, sizeof (constants), 0);

	// 1スレッドグループで幅16ドット、高さ16ドット処理するシェーダを用いているため、Dispatchの引数をこのようにする
	static const UINT kGroupDimension = 16;
	UINT dispatch_x = (static_cast<UINT>(width) + (kGroupDimension - 1)) / kGroupDimension;
	UINT dispatch_y = (static_cast<UINT>(height) + (kGroupDimension - 1)) / kGroupDimension;
	UINT dispatch_z = 1;
	device_context->Dispatch(dispatch_x, dispatch_y, dispatch_z);
}

void CopyDefaultTextureToStagingTexture(filter_t *filter)
{
	ID3D11DeviceContext *device_context = filter->p_sys->device_context_;
	ID3D11Texture2D *default_texture = filter->p_sys->default_texture_;
	ID3D11Texture2D *staging_texture = filter->p_sys->staging_texture_;

	device_context->CopyResource(staging_texture, default_texture);
}

bool CopyStagingTextureToPicture(filter_t *filter, picture_t *output_picture)
{
	ID3D11DeviceContext *device_context = filter->p_sys->device_context_;
	ID3D11Texture2D *staging_texture = filter->p_sys->staging_texture_;
	D3D11_MAPPED_SUBRESOURCE mapped;
	plane_t *plane = &output_picture->p[0];

	if (FAILED(device_context->Map(staging_texture, 0, D3D11_MAP_READ, 0, &mapped)))
		return false;

	uint8_t *dst = plane->p_pixels;
	uint8_t *src = reinterpret_cast<uint8_t *>(mapped.pData);
	for (int y=0; y<plane->i_visible_lines; ++y)
	{
		CopyMemory(dst, src, plane->i_visible_pitch);
		dst += plane->i_pitch;
		src += mapped.RowPitch;
	}

	device_context->Unmap(staging_texture, 0);

	return true;
}


vlc_module_begin()
set_shortname("FidelityFX CAS")
set_description("FidelityFX CAS")
set_capability("video filter", 0)
set_category(CAT_VIDEO)
set_subcategory(SUBCAT_VIDEO_VFILTER)

add_integer(kVarNameAdapter, 0, "Adapter", "Adapter-number (0 .. n-1)", false)
add_float_with_range(kVarNameSharpness, 0.8, 0.0, 1.0, "Sharpness", "Sharpness [0, 1]", false)
add_bool(kVarNameFp16prefer, false, "FP16", "FP16 is preferred use.", false)

add_shortcut("FidelityFX CAS")
set_callbacks(Open, Close)
vlc_module_end()
