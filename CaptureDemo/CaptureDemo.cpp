// CaptureDemo.cpp : 定义控制台应用程序的入口点。
//

#include "stdafx.h"


#include <dshow.h>
#include "qedit.h"
#include <iostream>
#include <objbase.h>
#include <atlconv.h>
#include <strmif.h>
#include <vidcap.h>         // For IKsTopologyInfo  
#include <ksproxy.h>        // For IKsControl  
#include <ks.h>
#include <ksmedia.h>
#include <Windows.h>

#pragma comment(lib,"strmiids.lib")
#pragma comment(lib,"strmbase.lib")

#define DEFAULT_VIDEO_WIDTH     640
#define DEFAULT_VIDEO_HEIGHT    480

// this object is a SEMI-COM object, and can only be created statically.

enum PLAYSTATE { Stopped, Paused, Running, Init };
PLAYSTATE psCurrent = Stopped;

IMediaControl *pMediaControl = NULL;
IMediaEvent *pMediaEvent = NULL;
IGraphBuilder *pGraphBuilder = NULL;
ICaptureGraphBuilder2 *pCaptureGraphBuilder2 = NULL;
IVideoWindow *pVideoWindow = NULL;
IMoniker *pMonikerVideo = NULL;
IBaseFilter *pVideoCaptureFilter = NULL;
IBaseFilter *pGrabberF = NULL;
ISampleGrabber *pSampleGrabber = NULL;

IBaseFilter *pGrabberStill = NULL;
ISampleGrabber *pSampleGrabberStill = NULL;
IBaseFilter *pNull = NULL;


//抓拍回调
class CSampleGrabberCB : public ISampleGrabberCB 
{
public:

	long Width;
	long Height;

	HANDLE BufferEvent;
	LONGLONG prev, step;
	DWORD lastTime;
	// Fake out any COM ref counting
	STDMETHODIMP_(ULONG) AddRef() { return 2; }
	STDMETHODIMP_(ULONG) Release() { return 1; }

	CSampleGrabberCB()
	{
		lastTime =0;
	}
	// Fake out any COM QI'ing
	STDMETHODIMP QueryInterface(REFIID riid, void ** ppv)
	{
		//CheckPointer(ppv,E_POINTER);

		if( riid == IID_ISampleGrabberCB || riid == IID_IUnknown ) 
		{
			*ppv = (void *) static_cast<ISampleGrabberCB*> ( this );
			return NOERROR;
		}    

		return E_NOINTERFACE;
	}

	STDMETHODIMP SampleCB( double SampleTime, IMediaSample * pSample )
	{
		return 0;
	}

	STDMETHODIMP BufferCB( double SampleTime, BYTE * pBuffer, long BufferSize )
	{
		//TODO 数据格式为YUY2，需要转换
		char FileName[256];
		sprintf_s(FileName, "capture_%d.yuv", (int)GetTickCount());
		FILE * out = fopen(FileName, "wb");
		fwrite(pBuffer, 1, BufferSize, out);
		fclose(out);
		return 0;
	}
};

//设置预览窗口位置
void SetupVideoWindow(void)
{
	pVideoWindow->put_Left(0); 
	pVideoWindow->put_Width(DEFAULT_VIDEO_WIDTH); 
	pVideoWindow->put_Top(0); 
	pVideoWindow->put_Height(DEFAULT_VIDEO_HEIGHT); 
	pVideoWindow->put_Caption(L"Video Window");
}

HRESULT GetInterfaces(void)
{
	HRESULT hr;

	//创建Filter Graph Manager.
	hr = CoCreateInstance (CLSID_FilterGraph, NULL, CLSCTX_INPROC,
		IID_IGraphBuilder, (void **) &pGraphBuilder);
	if (FAILED(hr))
		return hr;
	//创建Capture Graph Builder.
	hr = CoCreateInstance (CLSID_CaptureGraphBuilder2 , NULL, CLSCTX_INPROC,
		IID_ICaptureGraphBuilder2, (void **) &pCaptureGraphBuilder2);
	if (FAILED(hr))
		return hr;

	// IMediaControl接口，用来控制流媒体在Filter Graph中的流动，例如流媒体的启动和停止；
	hr = pGraphBuilder->QueryInterface(IID_IMediaControl,(LPVOID *) &pMediaControl);
	if (FAILED(hr))
		return hr;

	// IVideoWindow,用来显示预览视频
	hr = pGraphBuilder->QueryInterface(IID_IVideoWindow, (LPVOID *) &pVideoWindow);
	if (FAILED(hr))
		return hr;

	// IMediaEvent接口，该接口在Filter Graph发生一些事件时用来创建事件的标志信息并传送给应用程序
	hr = pGraphBuilder->QueryInterface(IID_IMediaEvent,(LPVOID *) &pMediaEvent);
	if (FAILED(hr))
		return hr;

	//创建用于预览的Sample Grabber Filter.
	hr = CoCreateInstance(CLSID_SampleGrabber, NULL, CLSCTX_INPROC_SERVER,
		IID_IBaseFilter, (void**)&pGrabberF);
	if (FAILED(hr))
		return hr;

	//获取ISampleGrabber接口，用于设置回调等相关信息
	hr = pGrabberF->QueryInterface(IID_ISampleGrabber, (void**)&pSampleGrabber);
	if (FAILED(hr)) 
		return hr;

	//创建用于抓拍的Sample Grabber Filter.
	hr = CoCreateInstance(CLSID_SampleGrabber, NULL, CLSCTX_INPROC_SERVER,IID_IBaseFilter, (void**)&pGrabberStill);
	if (FAILED(hr))
		return hr;

	//获取ISampleGrabber接口，用于设置回调等相关信息
	hr = pGrabberStill->QueryInterface(IID_ISampleGrabber, (void**)&pSampleGrabberStill);
	if (FAILED(hr)) 
		return hr;

	//创建Null Filter
	hr = CoCreateInstance(CLSID_NullRenderer,NULL,CLSCTX_INPROC_SERVER,IID_IBaseFilter,(void**)&pNull);
	if (FAILED(hr))
		return hr;
	return hr;
}

//关闭接口
void CloseInterfaces(void)
{
	if (pMediaControl)
		pMediaControl->StopWhenReady();
	psCurrent = Stopped;

	if(pVideoWindow) 
		pVideoWindow->put_Visible(OAFALSE);

	pMediaControl->Release();
	pGraphBuilder->Release();
	pVideoWindow->Release();
	pCaptureGraphBuilder2->Release();
}

//遍历设备，找到指定vidpid设备后打开设备
HRESULT InitMonikers()
{
	USES_CONVERSION;
	HRESULT hr;
	ULONG cFetched;

	//https://docs.microsoft.com/zh-cn/windows/desktop/DirectShow/selecting-a-capture-device
	//遍历设备
	ICreateDevEnum *pCreateDevEnum;
	hr = CoCreateInstance(CLSID_SystemDeviceEnum, NULL, CLSCTX_INPROC_SERVER, IID_ICreateDevEnum, (void**)&pCreateDevEnum);
	if (FAILED(hr))
	{
		printf("Failed to enumerate all video and audio capture devices!  hr=0x%x\n", hr);
		return hr;
	}

	IEnumMoniker *pEnumMoniker;
	hr = pCreateDevEnum->CreateClassEnumerator(CLSID_VideoInputDeviceCategory, &pEnumMoniker, 0);
	if (FAILED(hr) || !pEnumMoniker)
	{
		printf("Failed to create ClassEnumerator!  hr=0x%x\n", hr);
		return -1;
	}

	while (hr = pEnumMoniker->Next(1, &pMonikerVideo, &cFetched), hr == S_OK)
	{
		IPropertyBag *pPropBag;
		//BindToStorage之后就可以访问设备标识的属性集了。
		HRESULT hr = pMonikerVideo->BindToStorage(0, 0, IID_PPV_ARGS(&pPropBag));
		if (FAILED(hr))
		{
			pMonikerVideo->Release();
			continue;
		}

		VARIANT var;
		VariantInit(&var);
		//DevicePath中包含vidpid
		hr = pPropBag->Read(L"DevicePath", &var, 0);
		if (FAILED(hr))
		{
			VariantClear(&var);
			pMonikerVideo->Release();
			continue;
		}

		//比较是否是要使用的设备
		//TRACE("Device path: %S\n", var.bstrVal);
		std::string devpath = std::string(W2A(var.bstrVal));
		if (devpath.find("vid_06f8&pid_3015") == -1)
		{
			VariantClear(&var);
			pMonikerVideo->Release();
			continue;
		}
		VariantClear(&var);

		// BindToObject将某个设备标识绑定到一个DirectShow Filter，
		//     然后调用IFilterGraph::AddFilter加入到Filter Graph中，这个设备就可以参与工作了
		// 调用IMoniker::BindToObject建立一个和选择的device联合的filter，
		//      并且装载filter的属性(CLSID,FriendlyName, and DevicePath)。
		hr = pMonikerVideo->BindToObject(0, 0, IID_IBaseFilter, (void**)&pVideoCaptureFilter);
		if (FAILED(hr))
		{
			printf("Couldn't bind moniker to filter object!  hr=0x%x\n", hr);
			return hr;
		}

		pPropBag->Release();
		pMonikerVideo->Release();
	}

	pEnumMoniker->Release();
	return hr;
}

//https://docs.microsoft.com/zh-cn/windows/desktop/DirectShow/capturing-an-image-from-a-still-image-pin
//具体到本设备：
//USB Camera有两个Pin
//Capture pin和Still pin
//Capture pin用于视频流预览
//Still pin用于响应抓拍（可以软触发和硬件触发）
//想要使用Still pin，必须先连接上Capture pin，才能正常使用Still pin
HRESULT CaptureVideo()
{
	//DirectShow的接口使用了COM，所以首先要初始化com环境
	HRESULT hr = CoInitialize(NULL);

	//https://docs.microsoft.com/zh-cn/windows/desktop/DirectShow/about-the-capture-graph-builder
	//初始化接口
	hr = GetInterfaces();
	if (FAILED(hr))
	{
		printf("Failed to get video interfaces!  hr=0x%x\n", hr);
		return hr;
	}

	// 初始化 Capture Graph Builder.
	hr = pCaptureGraphBuilder2->SetFiltergraph(pGraphBuilder);
	if (FAILED(hr))
	{
		printf("Failed to attach the filter graph to the capture graph!  hr=0x%x\n", hr);
		return hr;
	}

	//检测设备
	hr = InitMonikers();
	if(FAILED(hr))
	{
		printf("Failed to InitMonikers!  hr=0x%x\n", hr);
		return hr;
	}

	//开始构建预览graph
	//加入设备抓拍filter
	hr = pGraphBuilder->AddFilter(pVideoCaptureFilter, L"Video Capture");
	if (FAILED(hr))
	{
		printf("Couldn't add video capture filter to graph!  hr=0x%x\n", hr);
		pVideoCaptureFilter->Release();
		return hr;
	}

	//加入DirectShow中自带的SampleGrabber Filter
	hr = pGraphBuilder->AddFilter(pGrabberF, L"Sample Grabber");
	if (FAILED(hr))
	{
		printf("Couldn't add sample grabber to graph!  hr=0x%x\n", hr);
		// Return an error.
	}

	//使用Capture Graph Builder构建预览 Graph
	hr = pCaptureGraphBuilder2->RenderStream(&PIN_CATEGORY_CAPTURE, &MEDIATYPE_Video, pVideoCaptureFilter, pGrabberF, 0 );
	if (FAILED(hr))
	{
		printf("Couldn't render video capture stream. The device may already be in use.  hr=0x%x\n", hr);
		pVideoCaptureFilter->Release();
		return hr;
	}

	//加入DirectShow中自带的SampleGrabber Filter
	hr = pGraphBuilder->AddFilter(pGrabberStill, L"Still Sample Grabber");
	if (FAILED(hr))
	{
		printf("Couldn't add sample grabber to graph!  hr=0x%x\n", hr);
		// Return an error.
	}

	//加入DirectShow中自带的NullRender Filter
	hr = pGraphBuilder->AddFilter(pNull, L"NullRender");
	if (FAILED(hr))
	{
		printf("Couldn't add null to graph!  hr=0x%x\n", hr);
		return hr;
	}

	//使用Capture Graph Builder构建抓拍 Graph
	hr = pCaptureGraphBuilder2->RenderStream(&PIN_CATEGORY_STILL, &MEDIATYPE_Video, pVideoCaptureFilter, pGrabberStill, pNull);
	if (FAILED(hr))
	{
		printf("Couldn't render video capture stream. The device may already be in use.  hr=0x%x\n", hr);
		pVideoCaptureFilter->Release();
		return hr;
	}

	//configure the Sample Grabber so that it buffers samples :
	hr = pSampleGrabberStill->SetOneShot(FALSE);
	hr = pSampleGrabberStill->SetBufferSamples(TRUE);

	//获取设备输出格式信息
	AM_MEDIA_TYPE mt;
	hr = pSampleGrabber->GetConnectedMediaType(&mt);
	if (FAILED(hr))
	{
		return -1;
	}
	VIDEOINFOHEADER * vih = (VIDEOINFOHEADER*)mt.pbFormat;
	CSampleGrabberCB *CB = new CSampleGrabberCB();
	if (!FAILED(hr))
	{
		CB->Width = vih->bmiHeader.biWidth;
		CB->Height = vih->bmiHeader.biHeight;
	}

	//设置触发抓拍后，调用的回调函数 0-调用SampleCB 1-BufferCB
	hr = pSampleGrabberStill->SetCallback(CB, 1);
	if (FAILED(hr))
	{
		printf("set still trigger call back failed\n");
	}

	//pVideoCaptureFilter->Release();

	//设置预览窗口大小位置
	SetupVideoWindow();

	//启动设备
	hr = pMediaControl->Run();
	if (FAILED(hr))
	{
		printf("Couldn't run the graph!  hr=0x%x\n", hr);
		return hr;
	}
	else 
		psCurrent = Running;

#if 1
	IKsTopologyInfo *pInfo = NULL;
	hr = pVideoCaptureFilter->QueryInterface(__uuidof(IKsTopologyInfo),(void **)&pInfo);
	if (SUCCEEDED(hr))
	{
		DWORD dwNumNodes = 0;
		DWORD dwINode = 3;
		pInfo->get_NumNodes(&dwNumNodes);
		for (int i = 0;i < dwNumNodes;i++)
		{
			GUID nodeType;
			pInfo->get_NodeType(i,&nodeType);
			dwINode = i;	
		}
			
	}

	IKsControl *pCtl = NULL;
	hr = pInfo->CreateNodeInstance(0,IID_IKsControl,(void **)&pCtl);
	//hr = pVideoCaptureFilter->QueryInterface(IID_IKsControl, (void **)&pCtl);
	if(FAILED(hr)) return (0);

	HANDLE hEvent = CreateEvent(NULL, FALSE, FALSE, NULL);
	if (!hEvent)
	{
		printf("CreateEvent failed\n");
		return -1;
	}
	
	KSEVENT Event;
	Event.Set = KSEVENTSETID_VIDCAPNotify;
	Event.Id = KSEVENT_VIDCAPTOSTI_EXT_TRIGGER;
	Event.Flags = KSEVENT_TYPE_ENABLE ;


	KSEVENTDATA EventData;

	EventData.NotificationType = KSEVENTF_EVENT_HANDLE;
	EventData.EventHandle.Event = hEvent;
	EventData.EventHandle.Reserved[0] = 0;
	EventData.EventHandle.Reserved[1] = 0;

	ULONG ulBytesReturned = 0L;
	// register for autoupdate events
	hr = pCtl->KsEvent(
		&Event, 
		sizeof(Event), 
		&EventData, 
		sizeof(KSEVENTDATA), 
		&ulBytesReturned);
	if (FAILED(hr))
	{
		printf("Failed to register for auto-update event : %x\n", hr);
		return -1;
	}

	// Wait for event for 5 seconds 
	DWORD dwError = WaitForSingleObject(hEvent, 50000);

	// cancel further notifications
	hr = pCtl->KsEvent(
		NULL, 
		0, 
		&EventData, 
		sizeof(KSEVENTDATA), 
		&ulBytesReturned);
	if (FAILED(hr))  printf("Cancel event returns : %x\n", hr);

	if ((dwError == WAIT_FAILED) || 
		(dwError == WAIT_ABANDONED) ||
		(dwError == WAIT_TIMEOUT))
	{
		printf("Wait failed : %d\n", dwError);
		return -1;
	} 
	printf("Wait returned : %d\n", dwError);
#endif

	return hr;
}

//停止预览
void StopPreview()
{
	pMediaControl->Stop();
	CloseInterfaces();
	CoUninitialize();
	psCurrent = Stopped;
}

int main()
{
	HRESULT hr;														
	char cmd;
	printf("p - Play Video\ns - Stop Video\nq - Quit\n\n");

	while (true)
	{
		std::cin >> cmd;
		switch(cmd)
		{
		case 'p': 
			{															
				printf("	Play Video!\n");
				hr = CaptureVideo();
				if (FAILED(hr))	
					printf("Error!");
			}
			break;
		case 's': 
			{															
				printf("	Stop Video!\n");
				if (psCurrent == Running) StopPreview();
				else printf ("Video already stopped.\n");
			}
			break;
		case 'q': 
			return 0;											
			break;
		default: printf("Unknown command!\n");
			break;
		}
	}
}