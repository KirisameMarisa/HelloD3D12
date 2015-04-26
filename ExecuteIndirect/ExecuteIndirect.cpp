#include <Windows.h>
#include <tchar.h>
#include <wrl/client.h>
#include <stdexcept>
#include <ppl.h>
#include <dxgi1_3.h>
#include <d3d12.h>
#include <d3dcompiler.h>

#include <DirectXMath.h>
using DirectX::XMFLOAT3; // for WaveFrontReader
#include "../Mesh/WaveFrontReader.h"

#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "d3d12.lib")
#pragma comment(lib, "d3dcompiler.lib")

using namespace std;
using namespace concurrency;
using namespace DirectX;
using Microsoft::WRL::ComPtr;

namespace
{
	const int WINDOW_WIDTH = 400;
	const int WINDOW_HEIGHT = 240;
	HWND g_mainWindowHandle = 0;
};

void CHK(HRESULT hr)
{
	if (FAILED(hr))
		throw runtime_error("HRESULT is failed value.");
}

class D3D
{
	ComPtr<IDXGIFactory2> mDxgiFactory;
	ComPtr<IDXGISwapChain1> mSwapChain;
	ComPtr<ID3D12Resource> mD3DBuffer;
	int mBufferWidth, mBufferHeight;
	UINT64 mFrameCount = 0;
	static const UINT MaxFrameLatency = 2;

	UINT mInstanceCount = 4;

	ID3D12Device* mDev;
	ComPtr<ID3D12CommandAllocator> mCmdAlloc[MaxFrameLatency];
	ComPtr<ID3D12CommandQueue> mCmdQueue;

	ComPtr<ID3D12GraphicsCommandList> mCmdList;
	ComPtr<ID3D12Fence> mFence;
	HANDLE mFenceEveneHandle = 0;

	ComPtr<ID3D12DescriptorHeap> mDescHeapRtv;
	ComPtr<ID3D12DescriptorHeap> mDescHeapDsv;
	ComPtr<ID3D12DescriptorHeap> mDescHeapCbvSrvUav[MaxFrameLatency];
	void* mCBUploadPtr = nullptr;

	ComPtr<ID3D12RootSignature> mRootSignature;
	ComPtr<ID3D12PipelineState> mPso;
	ComPtr<ID3D12Resource> mVB;
	D3D12_VERTEX_BUFFER_VIEW mVBView = {};
	D3D12_INDEX_BUFFER_VIEW mIBView = {};
	UINT mIndexCount = 0;
	UINT mVBIndexOffset = 0;
	ComPtr<ID3D12Resource> mDB;
	ComPtr<ID3D12Resource> mCB;

	ComPtr<ID3D12CommandSignature> mCmdSignature;
	ComPtr<ID3D12Resource> mIndirectCmdBuf;
	ComPtr<ID3D12Resource> mIndirectCmdBufOnDefaultHeap;
	void* mIndirectCmdBufUploadPtr = nullptr;
	UINT mIndirectCmdBufStride = 0;

public:
	D3D(int width, int height, HWND hWnd)
		: mBufferWidth(width), mBufferHeight(height), mDev(nullptr)
	{
		{
#if _DEBUG
			CHK(CreateDXGIFactory2(DXGI_CREATE_FACTORY_DEBUG, IID_PPV_ARGS(mDxgiFactory.ReleaseAndGetAddressOf())));
#else
			CHK(CreateDXGIFactory2(0, IID_PPV_ARGS(mDxgiFactory.ReleaseAndGetAddressOf())));
#endif /* _DEBUG */
		}

		D3D12_CREATE_DEVICE_FLAG createFlag = D3D12_CREATE_DEVICE_NONE;
#if _DEBUG
		createFlag = D3D12_CREATE_DEVICE_DEBUG;
#endif /* _DEBUG */
		ID3D12Device* dev;
		CHK(D3D12CreateDevice(
			nullptr,
			D3D_DRIVER_TYPE_WARP,
			//D3D_DRIVER_TYPE_HARDWARE,
			createFlag,
			D3D_FEATURE_LEVEL_11_1,
			D3D12_SDK_VERSION,
			IID_PPV_ARGS(&dev)));
		mDev = dev;

		for (auto& a : mCmdAlloc)
		{
			CHK(mDev->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(a.ReleaseAndGetAddressOf())));
		}

		D3D12_COMMAND_QUEUE_DESC queueDesc = {};
		queueDesc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
		CHK(mDev->CreateCommandQueue(&queueDesc, IID_PPV_ARGS(mCmdQueue.ReleaseAndGetAddressOf())));

		DXGI_SWAP_CHAIN_DESC1 scDesc = {};
		scDesc.Width = width;
		scDesc.Height = height;
		scDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
		scDesc.SampleDesc.Count = 1;
		scDesc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
		scDesc.BufferCount = 1;
		scDesc.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;
		CHK(mDxgiFactory->CreateSwapChainForHwnd(mCmdQueue.Get(), hWnd, &scDesc, nullptr, nullptr, mSwapChain.ReleaseAndGetAddressOf()));

		CHK(mDev->CreateCommandList(
			0,
			D3D12_COMMAND_LIST_TYPE_DIRECT,
			mCmdAlloc[0].Get(),
			nullptr,
			IID_PPV_ARGS(mCmdList.ReleaseAndGetAddressOf())));
		mCmdList->Close();

		CHK(mDev->CreateFence(0, D3D12_FENCE_MISC_NONE, IID_PPV_ARGS(mFence.ReleaseAndGetAddressOf())));

		mFenceEveneHandle = CreateEvent(nullptr, FALSE, FALSE, nullptr);

		CHK(mSwapChain->GetBuffer(0, IID_PPV_ARGS(mD3DBuffer.ReleaseAndGetAddressOf())));
		mD3DBuffer->SetName(L"SwapChain_Buffer");

		{
			D3D12_DESCRIPTOR_HEAP_DESC desc = {};
			desc.Type = D3D12_RTV_DESCRIPTOR_HEAP;
			desc.NumDescriptors = 10;
			//desc.Flags = D3D12_DESCRIPTOR_HEAP_SHADER_VISIBLE;
			desc.NodeMask = 0;
			CHK(mDev->CreateDescriptorHeap(&desc, IID_PPV_ARGS(mDescHeapRtv.ReleaseAndGetAddressOf())));

			desc.Type = D3D12_DSV_DESCRIPTOR_HEAP;
			desc.NumDescriptors = 10;
			//desc.Flags = D3D12_DESCRIPTOR_HEAP_SHADER_VISIBLE;
			desc.NodeMask = 0;
			CHK(mDev->CreateDescriptorHeap(&desc, IID_PPV_ARGS(mDescHeapDsv.ReleaseAndGetAddressOf())));

			desc.Type = D3D12_CBV_SRV_UAV_DESCRIPTOR_HEAP;
			desc.NumDescriptors = 100;
			desc.Flags = D3D12_DESCRIPTOR_HEAP_SHADER_VISIBLE;
			desc.NodeMask = 0;
			for (auto& c : mDescHeapCbvSrvUav)
			{
				CHK(mDev->CreateDescriptorHeap(&desc, IID_PPV_ARGS(c.ReleaseAndGetAddressOf())));
			}
		}

		mDev->CreateRenderTargetView(mD3DBuffer.Get(), nullptr, mDescHeapRtv->GetCPUDescriptorHandleForHeapStart());

		{
			//D3D12_DESCRIPTOR_RANGE descRange1[1];
			//descRange1[0].Init(D3D12_DESCRIPTOR_RANGE_CBV, 1, 0);

			D3D12_ROOT_PARAMETER rootParam[1];
			rootParam[0].InitAsConstantBufferView(0);

			ID3D10Blob *sig, *info;
			D3D12_ROOT_SIGNATURE rootSigDesc = D3D12_ROOT_SIGNATURE();
			rootSigDesc.NumParameters = ARRAYSIZE(rootParam);
			rootSigDesc.NumStaticSamplers = 0;
			rootSigDesc.pParameters = rootParam;
			rootSigDesc.pStaticSamplers = nullptr;
			rootSigDesc.Flags = D3D12_ROOT_SIGNATURE_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;
			CHK(D3D12SerializeRootSignature(&rootSigDesc, D3D_ROOT_SIGNATURE_V1, &sig, &info));
			mDev->CreateRootSignature(
				0,
				sig->GetBufferPointer(),
				sig->GetBufferSize(),
				IID_PPV_ARGS(mRootSignature.ReleaseAndGetAddressOf()));
			sig->Release();
		}

		ID3D10Blob *vs, *ps;
		{
			ID3D10Blob *info;
			UINT flag = 0;
#if _DEBUG
			flag |= D3DCOMPILE_DEBUG;
#endif /* _DEBUG */
			CHK(D3DCompileFromFile(L"../Mesh/Mesh.hlsl", nullptr, nullptr, "VSMain", "vs_5_0", flag, 0, &vs, &info));
			CHK(D3DCompileFromFile(L"../Mesh/Mesh.hlsl", nullptr, nullptr, "PSMain", "ps_5_0", flag, 0, &ps, &info));
		}
		D3D12_INPUT_ELEMENT_DESC inputLayout[] = {
			{ "POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0, D3D12_INPUT_PER_VERTEX_DATA, 0 },
			{ "NORMAL", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 12, D3D12_INPUT_PER_VERTEX_DATA, 0 },
			{ "TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 24, D3D12_INPUT_PER_VERTEX_DATA, 0 },
		};
		D3D12_GRAPHICS_PIPELINE_STATE_DESC psoDesc = {};
		psoDesc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
		psoDesc.InputLayout.NumElements = 3;
		psoDesc.InputLayout.pInputElementDescs = inputLayout;
		psoDesc.IndexBufferProperties = D3D12_INDEX_BUFFER_STRIP_CUT_VALUE_DISABLED;
		psoDesc.pRootSignature = mRootSignature.Get();
		psoDesc.VS.pShaderBytecode = vs->GetBufferPointer();
		psoDesc.VS.BytecodeLength = vs->GetBufferSize();
		psoDesc.PS.pShaderBytecode = ps->GetBufferPointer();
		psoDesc.PS.BytecodeLength = ps->GetBufferSize();
		psoDesc.RasterizerState = CD3D12_RASTERIZER_DESC(D3D12_DEFAULT);
		psoDesc.BlendState = CD3D12_BLEND_DESC(D3D12_DEFAULT);
		psoDesc.DepthStencilState.DepthEnable = true;
		psoDesc.DepthStencilState.DepthFunc = D3D12_COMPARISON_LESS_EQUAL;
		psoDesc.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ALL;
		psoDesc.DepthStencilState.StencilEnable = false;
		psoDesc.SampleMask = UINT_MAX;
		psoDesc.NumRenderTargets = 1;
		psoDesc.RTVFormats[0] = DXGI_FORMAT_R8G8B8A8_UNORM;
		psoDesc.DSVFormat = DXGI_FORMAT_D32_FLOAT;
		psoDesc.SampleDesc.Count = 1;
		CHK(mDev->CreateGraphicsPipelineState(&psoDesc, IID_PPV_ARGS(mPso.ReleaseAndGetAddressOf())));
		vs->Release();
		ps->Release();

		WaveFrontReader<uint16_t> mesh;
		CHK(mesh.Load(L"../Mesh/teapot.obj"));

		mIndexCount = static_cast<UINT>(mesh.indices.size());
		mVBIndexOffset = static_cast<UINT>(sizeof(mesh.vertices[0]) * mesh.vertices.size());
		UINT IBSize = static_cast<UINT>(sizeof(mesh.indices[0]) * mIndexCount);

		void* vbData = mesh.vertices.data();
		void* ibData = mesh.indices.data();
		CHK(mDev->CreateCommittedResource(
			&CD3D12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_UPLOAD),
			D3D12_HEAP_MISC_NONE,
			&CD3D12_RESOURCE_DESC::Buffer(sizeof(mVBIndexOffset) + sizeof(IBSize)),
			D3D12_RESOURCE_USAGE_GENERIC_READ,
			nullptr,
			IID_PPV_ARGS(mVB.ReleaseAndGetAddressOf())));
		mVB->SetName(L"VertexBuffer");
		char* vbUploadPtr = nullptr;
		CHK(mVB->Map(0, nullptr, reinterpret_cast<void**>(&vbUploadPtr)));
		memcpy_s(vbUploadPtr, mVBIndexOffset, vbData, mVBIndexOffset);
		memcpy_s(vbUploadPtr + mVBIndexOffset, IBSize, ibData, IBSize);
		mVB->Unmap(0, nullptr);

		mVBView.BufferLocation = mVB->GetGPUVirtualAddress();
		mVBView.StrideInBytes = sizeof(mesh.vertices[0]);
		mVBView.SizeInBytes = mVBIndexOffset;
		mIBView.BufferLocation = mVB->GetGPUVirtualAddress() + mVBIndexOffset;
		mIBView.Format = DXGI_FORMAT_R16_UINT;
		mIBView.SizeInBytes = IBSize;

		D3D12_RESOURCE_DESC resourceDesc = CD3D12_RESOURCE_DESC::Tex2D(
			DXGI_FORMAT_R32_TYPELESS, mBufferWidth, mBufferHeight, 1, 1, 1, 0, D3D12_RESOURCE_MISC_ALLOW_DEPTH_STENCIL,
			D3D12_TEXTURE_LAYOUT_UNKNOWN, 0);
		D3D12_CLEAR_VALUE dsvClearValue;
		dsvClearValue.Format = DXGI_FORMAT_D32_FLOAT;
		dsvClearValue.DepthStencil.Depth = 1.0f;
		dsvClearValue.DepthStencil.Stencil = 0;
		CHK(mDev->CreateCommittedResource(
			&CD3D12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_DEFAULT), // No need to read/write by CPU
			D3D12_HEAP_MISC_NONE,
			&resourceDesc,
			D3D12_RESOURCE_USAGE_DEPTH,
			&dsvClearValue,
			IID_PPV_ARGS(mDB.ReleaseAndGetAddressOf())));
		mDB->SetName(L"DepthTexture");

		D3D12_DEPTH_STENCIL_VIEW_DESC dsvDesc = {};
		dsvDesc.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2D;
		dsvDesc.Format = DXGI_FORMAT_D32_FLOAT;
		dsvDesc.Texture2D.MipSlice = 0;
		dsvDesc.Flags = D3D12_DSV_NONE;
		mDev->CreateDepthStencilView(mDB.Get(), &dsvDesc, mDescHeapDsv->GetCPUDescriptorHandleForHeapStart());

#define CB_SIZE 128
#define CB_ALIGNED_SIZE ((CB_SIZE + D3D12_CONSTANT_BUFFER_DATA_PLACEMENT_ALIGNMENT - 1) & ~(D3D12_CONSTANT_BUFFER_DATA_PLACEMENT_ALIGNMENT - 1))
		UINT cbSize = CB_ALIGNED_SIZE;
		CHK(mDev->CreateCommittedResource(
			&CD3D12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_UPLOAD),
			D3D12_HEAP_MISC_NONE,
			&CD3D12_RESOURCE_DESC::Buffer(cbSize * MaxFrameLatency * mInstanceCount),
			D3D12_RESOURCE_USAGE_GENERIC_READ,
			nullptr,
			IID_PPV_ARGS(mCB.ReleaseAndGetAddressOf())));
		mCB->SetName(L"ConstantBuffer");
		auto cbvDescHeapIncSize = mDev->GetDescriptorHandleIncrementSize(D3D12_CBV_SRV_UAV_DESCRIPTOR_HEAP);
		for (auto i = 0u; i < MaxFrameLatency; ++i)
		{
			for (auto j = 0u; j < mInstanceCount; ++j)
			{
				D3D12_CONSTANT_BUFFER_VIEW_DESC cbvDesc = {};
				cbvDesc.BufferLocation = mCB->GetGPUVirtualAddress() + (i * mInstanceCount + j) * cbSize;
				cbvDesc.SizeInBytes = cbSize;
				mDev->CreateConstantBufferView(
					&cbvDesc,
					mDescHeapCbvSrvUav[i]->GetCPUDescriptorHandleForHeapStart().MakeOffsetted(j * cbvDescHeapIncSize));
			}
		}
		CHK(mCB->Map(0, nullptr, reinterpret_cast<void**>(&mCBUploadPtr)));

		{
			D3D12_INDIRECT_PARAMETER param[2];
			param[0].Type = D3D12_INDIRECT_PARAMETER_CONSTANT_BUFFER_VIEW; // Bytes 0:7
			param[0].ConstantBufferView.RootParameterIndex = 0;
			param[1].Type = D3D12_INDIRECT_PARAMETER_DRAW_INDEXED; // Bytes 8:27
			D3D12_COMMAND_SIGNATURE cmdSignatureDesc = {};
			cmdSignatureDesc.ByteStride = 28;
			cmdSignatureDesc.ParameterCount = ARRAYSIZE(param);
			cmdSignatureDesc.pParameters = param;
			CHK(mDev->CreateCommandSignature(&cmdSignatureDesc, mRootSignature.Get(), IID_PPV_ARGS(mCmdSignature.ReleaseAndGetAddressOf())));

			mIndirectCmdBufStride = cmdSignatureDesc.ByteStride;

			CHK(mDev->CreateCommittedResource(
				&CD3D12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_UPLOAD),
				D3D12_HEAP_MISC_NONE,
				&CD3D12_RESOURCE_DESC::Buffer(cmdSignatureDesc.ByteStride * MaxFrameLatency * mInstanceCount),
				D3D12_RESOURCE_USAGE_GENERIC_READ,
				nullptr,
				IID_PPV_ARGS(mIndirectCmdBuf.ReleaseAndGetAddressOf())));
			CHK(mIndirectCmdBuf->Map(0, nullptr, reinterpret_cast<void**>(&mIndirectCmdBufUploadPtr)));

			CHK(mDev->CreateCommittedResource(
				&CD3D12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_DEFAULT),
				D3D12_HEAP_MISC_NONE,
				&CD3D12_RESOURCE_DESC::Buffer(cmdSignatureDesc.ByteStride * MaxFrameLatency * mInstanceCount),
				D3D12_RESOURCE_USAGE_INDIRECT_ARGUMENT,
				nullptr,
				IID_PPV_ARGS(mIndirectCmdBufOnDefaultHeap.ReleaseAndGetAddressOf())));
		}
	}
	~D3D()
	{
		mCB->Unmap(0, nullptr);
		mIndirectCmdBuf->Unmap(0, nullptr);
		CloseHandle(mFenceEveneHandle);
	}
	ID3D12Device* GetDevice() const
	{
		return mDev;
	}
	void Draw()
	{
		mFrameCount++;

		int cmdIndex = mFrameCount % MaxFrameLatency;
		auto* cmdQueue = mCmdQueue.Get();
		auto* cmdList = mCmdList.Get();

		// Wait untill next queue be freed
		if (mFrameCount > MaxFrameLatency)
		{
			mFence->SetEventOnCompletion(mFrameCount - MaxFrameLatency, mFenceEveneHandle);
			DWORD wait = WaitForSingleObject(mFenceEveneHandle, 10000);
			if (wait != WAIT_OBJECT_0)
				throw runtime_error("Failed WaitForSingleObject().");

			CHK(mCmdAlloc[cmdIndex]->Reset());
		}

		CHK(cmdList->Reset(mCmdAlloc[cmdIndex].Get(), nullptr));

		// Upload constant buffer
		{
			static float rot = 0.0f;
			rot += 1.0f;
			if (rot >= 360.0f) rot = 0.0f;

			for (auto tid = 0u; tid < mInstanceCount; tid++)
			{
				XMMATRIX worldMat, viewMat, projMat;
				worldMat = XMMatrixIdentity();
				worldMat *= XMMatrixScaling(0.5f, 0.5f, 0.5f);
				worldMat *= XMMatrixRotationY(XMConvertToRadians(rot));
				worldMat *= XMMatrixTranslation(0.5f * ((tid & 1) ? 1 : -1), 0.2f + 0.5f * ((tid & 2) ? -1 : 1), 0);
				viewMat = XMMatrixLookAtLH({ 0, 0.5f, -1.5f }, { 0, 0.5f, 0 }, { 0, 1, 0 });
				projMat = XMMatrixPerspectiveFovLH(45, (float)mBufferWidth / mBufferHeight, 0.01f, 50.0f);
				auto mvpMat = XMMatrixTranspose(worldMat * viewMat * projMat);

				auto worldTransMat = XMMatrixTranspose(worldMat);

				// mCBUploadPtr is Write-Combine memory
				// Shift offset to guarantee that the pointer has not referred by executing command list.
				char* ptr = reinterpret_cast<char*>(mCBUploadPtr) + CB_ALIGNED_SIZE * (cmdIndex * mInstanceCount + tid);
				memcpy_s(ptr, 64, &mvpMat, 64);
				memcpy_s(ptr + 64, 64, &worldTransMat, 64);
			}
		}

		// Upload indirect parameters
		{
			// transition
			setResourceBarrier(cmdList, mIndirectCmdBufOnDefaultHeap.Get(), D3D12_RESOURCE_USAGE_INDIRECT_ARGUMENT, D3D12_RESOURCE_USAGE_COPY_DEST);

			// set parameters on upload heap
			UINT* ptr = reinterpret_cast<UINT*>(mIndirectCmdBufUploadPtr) + mIndirectCmdBufStride * (cmdIndex * mInstanceCount);
			for (auto tid = 0u; tid < mInstanceCount; tid++)
			{
				// Bytes 0:7 - D3D12_INDIRECT_PARAMETER_CONSTANT_BUFFER_VIEW
				*reinterpret_cast<D3D12_GPU_VIRTUAL_ADDRESS*>(ptr)
					= mCB->GetGPUVirtualAddress() + CB_ALIGNED_SIZE * (cmdIndex * mInstanceCount + tid);
				// Bytes 8:27 - D3D12_INDIRECT_PARAMETER_DRAW_INDEXED
				ptr[2] = mIndexCount;
				ptr[3] = 1;
				ptr[4] = 0;
				ptr[5] = 0;
				ptr[6] = 0;
			}

			// copy parameters to default heap
			//cmdList->CopyBufferRegion(mIndirectCmdBufOnDefaultHeap.Get(),
			//							mIndirectCmdBufStride * mInstanceCount * cmdIndex,
			//							mIndirectCmdBuf.Get(),
			//							mIndirectCmdBufStride * mInstanceCount * cmdIndex,
			//							mIndirectCmdBufStride * mInstanceCount,
			//							D3D12_COPY_NONE);
			cmdList->CopyResource(mIndirectCmdBufOnDefaultHeap.Get(), mIndirectCmdBuf.Get());

			// transition
			setResourceBarrier(cmdList, mIndirectCmdBufOnDefaultHeap.Get(), D3D12_RESOURCE_USAGE_COPY_DEST, D3D12_RESOURCE_USAGE_INDIRECT_ARGUMENT);
		}

		// Set queue flushed event
		CHK(mFence->SetEventOnCompletion(mFrameCount, mFenceEveneHandle));

		auto descHandleRtv = mDescHeapRtv->GetCPUDescriptorHandleForHeapStart();
		auto descHandleDsv = mDescHeapDsv->GetCPUDescriptorHandleForHeapStart();

		// Barrier Present -> RenderTarget
		setResourceBarrier(cmdList, mD3DBuffer.Get(), D3D12_RESOURCE_USAGE_PRESENT, D3D12_RESOURCE_USAGE_RENDER_TARGET);

		// Clear DepthTexture
		cmdList->ClearDepthStencilView(descHandleDsv, D3D12_CLEAR_DEPTH, 1.0f, 0, nullptr, 0);

		// Clear
		{
			float clearColor[4] = { 0.1f, 0.2f, 0.3f, 1.0f };
			cmdList->ClearRenderTargetView(descHandleRtv, clearColor, nullptr, 0);
		}

		cmdList->SetRenderTargets(&descHandleRtv, true, 1, &descHandleDsv);

		// Viewport & Scissor
		D3D12_VIEWPORT viewport = {};
		viewport.Width = (float)mBufferWidth;
		viewport.Height = (float)mBufferHeight;
		viewport.MinDepth = 0.0f;
		viewport.MaxDepth = 1.0f;
		cmdList->RSSetViewports(1, &viewport);
		D3D12_RECT scissor = {};
		scissor.right = (LONG)mBufferWidth;
		scissor.bottom = (LONG)mBufferHeight;
		cmdList->RSSetScissorRects(1, &scissor);

		auto cbvDescHeapIncSize = mDev->GetDescriptorHandleIncrementSize(D3D12_CBV_SRV_UAV_DESCRIPTOR_HEAP);

#if 0
		for (auto tid = 0u; tid < mInstanceCount; tid++)
		{
			// Draw
			cmdList->SetGraphicsRootSignature(mRootSignature.Get());
			//ID3D12DescriptorHeap* descHeaps[] = { mDescHeapCbvSrvUav[cmdIndex].Get() };
			//cmdList->SetDescriptorHeaps(descHeaps, ARRAYSIZE(descHeaps));
			{
				//cmdList->SetGraphicsRootDescriptorTable(0,
				//	mDescHeapCbvSrvUav[cmdIndex]->GetGPUDescriptorHandleForHeapStart().MakeOffsetted(tid * cbvDescHeapIncSize));
				cmdList->SetGraphicsRootConstantBufferView(0,
					mCB->GetGPUVirtualAddress() + CB_ALIGNED_SIZE * (cmdIndex * mInstanceCount + tid));
				cmdList->SetPipelineState(mPso.Get());
				cmdList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
				cmdList->SetVertexBuffers(0, &mVBView, 1);
				cmdList->SetIndexBuffer(&mIBView);
				cmdList->DrawIndexedInstanced(mIndexCount, 1, 0, 0, 0);
			}
		}
#else
		// Execute indirect
		cmdList->SetGraphicsRootSignature(mRootSignature.Get());
		cmdList->SetPipelineState(mPso.Get());
		cmdList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
		cmdList->SetVertexBuffers(0, &mVBView, 1);
		cmdList->SetIndexBuffer(&mIBView);
		cmdList->ExecuteIndirect(mCmdSignature.Get(),
									1,
									mIndirectCmdBufOnDefaultHeap.Get(),
									mIndirectCmdBufStride * mInstanceCount * cmdIndex,
									nullptr,
									0);
#endif

		// Barrier RenderTarget -> Present
		setResourceBarrier(cmdList, mD3DBuffer.Get(), D3D12_RESOURCE_USAGE_RENDER_TARGET, D3D12_RESOURCE_USAGE_PRESENT);

		// Exec
		CHK(cmdList->Close());
		ID3D12CommandList* const cmdLists = cmdList;
		cmdQueue->ExecuteCommandLists(1, &cmdLists);
		CHK(cmdQueue->Signal(mFence.Get(), mFrameCount));

		// Present
		CHK(mSwapChain->Present(1, 0));
	}

private:
	void setResourceBarrier(ID3D12GraphicsCommandList* commandList, ID3D12Resource* res, UINT before, UINT after)
	{
		D3D12_RESOURCE_BARRIER_DESC desc = {};
		desc.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
		desc.Transition.pResource = res;
		desc.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
		desc.Transition.StateBefore = before;
		desc.Transition.StateAfter = after;
		desc.Transition.Flags = D3D12_RESOURCE_TRANSITION_BARRIER_NONE;
		commandList->ResourceBarrier(1, &desc);
	}
};

LRESULT CALLBACK WndProc(HWND hWnd, UINT message, WPARAM wParam, LPARAM lParam)
{
	PAINTSTRUCT ps;
	HDC hdc;

	switch (message) {
	case WM_KEYDOWN:
		if (wParam == VK_ESCAPE) {
			PostMessage(hWnd, WM_DESTROY, 0, 0);
			return 0;
		}
		break;

	case WM_PAINT:
		hdc = BeginPaint(hWnd, &ps);
		EndPaint(hWnd, &ps);
		break;

	case WM_DESTROY:
		PostQuitMessage(0);
		break;

	default:
		return DefWindowProc(hWnd, message, wParam, lParam);
	}
	return 0;
}

static HWND setupWindow(int width, int height)
{
	WNDCLASSEX wcex;
	wcex.cbSize = sizeof(WNDCLASSEX);
	wcex.style = CS_HREDRAW | CS_VREDRAW;
	wcex.lpfnWndProc = WndProc;
	wcex.cbClsExtra = 0;
	wcex.cbWndExtra = 0;
	wcex.hInstance = (HMODULE)GetModuleHandle(0);
	wcex.hIcon = nullptr;
	wcex.hCursor = LoadCursor(nullptr, IDC_ARROW);
	wcex.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
	wcex.lpszMenuName = nullptr;
	wcex.lpszClassName = _T("WindowClass");
	wcex.hIconSm = nullptr;
	if (!RegisterClassEx(&wcex)) {
		throw runtime_error("RegisterClassEx()");
	}

	RECT rect = { 0, 0, width, height };
	AdjustWindowRect(&rect, WS_OVERLAPPEDWINDOW, FALSE);
	const int windowWidth = (rect.right - rect.left);
	const int windowHeight = (rect.bottom - rect.top);

	HWND hWnd = CreateWindow(_T("WindowClass"), _T("Window"),
		WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, 0, windowWidth, windowHeight,
		nullptr, nullptr, nullptr, nullptr);
	if (!hWnd) {
		throw runtime_error("CreateWindow()");
	}

	return hWnd;
}

int WINAPI WinMain(HINSTANCE, HINSTANCE, LPSTR, int)
{
	MSG msg;
	ZeroMemory(&msg, sizeof msg);

	ID3D12Device* dev = nullptr;

#ifdef NDEBUG
	try
#endif
	{
		g_mainWindowHandle = setupWindow(WINDOW_WIDTH, WINDOW_HEIGHT);
		ShowWindow(g_mainWindowHandle, SW_SHOW);
		UpdateWindow(g_mainWindowHandle);

		D3D d3d(WINDOW_WIDTH, WINDOW_HEIGHT, g_mainWindowHandle);
		dev = d3d.GetDevice();

		while (msg.message != WM_QUIT) {
			BOOL r = PeekMessage(&msg, nullptr, 0, 0, PM_REMOVE);
			if (r == 0) {
				d3d.Draw();
			}
			else {
				DispatchMessage(&msg);
			}
		}
	}
#ifdef NDEBUG
	catch (std::exception &e) {
		MessageBoxA(g_mainWindowHandle, e.what(), "Exception occuured.", MB_ICONSTOP);
	}
#endif

	if (dev)
		dev->Release();

	return static_cast<int>(msg.wParam);
}
