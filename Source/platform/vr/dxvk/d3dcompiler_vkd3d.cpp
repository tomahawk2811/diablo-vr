/**
 * @file d3dcompiler_vkd3d.cpp
 *
 * D3DCompile for builds without Direct3D's compiler DLL, on vkd3d's HLSL compiler.
 * Only what the renderer asks of it: one source, no macros, no includes.
 */
#include <atomic>
#include <cstdlib>
#include <cstring>

#include <d3dcompiler.h>
#include <vkd3d_shader.h>

namespace {

class Blob final : public ID3DBlob {
public:
	Blob(const void *data, size_t size)
	    : data_(std::malloc(size))
	    , size_(size)
	{
		std::memcpy(data_, data, size);
	}

	HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void **object) override
	{
		if (riid != __uuidof(IUnknown) && riid != __uuidof(ID3DBlob)) {
			*object = nullptr;
			return E_NOINTERFACE;
		}
		AddRef();
		*object = this;
		return S_OK;
	}
	ULONG STDMETHODCALLTYPE AddRef() override { return ++references_; }
	ULONG STDMETHODCALLTYPE Release() override
	{
		const ULONG left = --references_;
		if (left == 0)
			delete this;
		return left;
	}
	void *STDMETHODCALLTYPE GetBufferPointer() override { return data_; }
	SIZE_T STDMETHODCALLTYPE GetBufferSize() override { return size_; }

private:
	~Blob() { std::free(data_); }

	void *data_;
	size_t size_;
	std::atomic<ULONG> references_ { 1 };
};

} // namespace

extern "C" HRESULT WINAPI D3DCompile(LPCVOID data, SIZE_T dataSize, LPCSTR sourceName, const D3D_SHADER_MACRO *defines, ID3DInclude *include,
    LPCSTR entryPoint, LPCSTR target, UINT flags, UINT effectFlags, ID3DBlob **code, ID3DBlob **errorMessages)
{
	if (errorMessages != nullptr)
		*errorMessages = nullptr;
	if (code == nullptr || defines != nullptr || include != nullptr)
		return E_INVALIDARG;
	*code = nullptr;

	// What d3dcompiler_47 does: globals that are const but not static are still shader constants.
	vkd3d_shader_compile_option options[3] = {
		{ VKD3D_SHADER_COMPILE_OPTION_API_VERSION, VKD3D_SHADER_API_VERSION_2_1 },
		{ VKD3D_SHADER_COMPILE_OPTION_BACKWARD_COMPATIBILITY, VKD3D_SHADER_COMPILE_OPTION_CONST_GLOBAL_UNIFORMS },
		{ VKD3D_SHADER_COMPILE_OPTION_STRIP_DEBUG, 1 },
	};
	vkd3d_shader_hlsl_source_info hlslInfo {};
	hlslInfo.type = VKD3D_SHADER_STRUCTURE_TYPE_HLSL_SOURCE_INFO;
	hlslInfo.entry_point = entryPoint;
	hlslInfo.profile = target;
	vkd3d_shader_compile_info info {};
	info.type = VKD3D_SHADER_STRUCTURE_TYPE_COMPILE_INFO;
	info.next = &hlslInfo;
	info.source.code = data;
	info.source.size = dataSize;
	info.source_type = VKD3D_SHADER_SOURCE_HLSL;
	info.target_type = VKD3D_SHADER_TARGET_DXBC_TPF;
	info.options = options;
	info.option_count = (flags & D3DCOMPILE_DEBUG) != 0 ? 2 : 3;
	info.log_level = VKD3D_SHADER_LOG_WARNING;
	info.source_name = sourceName;

	vkd3d_shader_code byteCode {};
	char *messages = nullptr;
	const int result = vkd3d_shader_compile(&info, &byteCode, &messages);
	if (messages != nullptr && errorMessages != nullptr && (result < 0 || messages[0] != '\0'))
		*errorMessages = new Blob(messages, std::strlen(messages) + 1);
	vkd3d_shader_free_messages(messages);
	if (result < 0)
		return E_FAIL;
	*code = new Blob(byteCode.code, byteCode.size);
	vkd3d_shader_free_shader_code(&byteCode);
	return S_OK;
}
