#define _WIN32_WINNT 0x0600 
#include <tchar.h>
#include <windows.h>
#include <strsafe.h>

#define GRS_ALLOC(sz)		HeapAlloc(GetProcessHeap(),0,sz)
#define GRS_CALLOC(sz)		HeapAlloc(GetProcessHeap(),HEAP_ZERO_MEMORY,sz)
#define GRS_SAFEFREE(p)		if(NULL != p){HeapFree(GetProcessHeap(),0,p);p=NULL;}

#define GRS_USEPRINTF() TCHAR pBuf[1024] = {}
#define GRS_PRINTF(...) \
    StringCchPrintf(pBuf,1024,__VA_ARGS__);\
    WriteConsole(GetStdHandle(STD_OUTPUT_HANDLE),pBuf,lstrlen(pBuf),NULL,NULL);

#define GRS_ASSERT(s) if(!(s)) {::DebugBreak();}

VOID GetAppPath(LPTSTR pszBuffer)
{
    DWORD dwLen = 0;
    if(0 == (dwLen = ::GetModuleFileName(NULL,pszBuffer,MAX_PATH)))
    {
        return;			
    }
    DWORD i = dwLen;
    for(; i > 0; i -- )
    {
        if( '\\' == pszBuffer[i] )
        {
            pszBuffer[i + 1] = '\0';
            break;
        }
    }
}

#define GRS_BEGINTHREAD(Fun,Param) CreateThread(NULL,0,(LPTHREAD_START_ROUTINE)Fun,Param,0,NULL)

#define GRS_MAXWRITEPERTHREAD 100 //每个线程最大写入次数
#define GRS_MAXWTHREAD		  20  //写入线程数量

#define GRS_OP_READ		0x1		//读取操作
#define GRS_OP_WRITE	0x2		//写入操作

struct ST_MY_OVERLAPPED
{
    OVERLAPPED m_ol;				//Overlapped 结构,不一定非得是第一个成员
    HANDLE	   m_hFile;				//操作的文件句柄
    DWORD	   m_dwOp;				//操作类型GRS_OP_READ/GRS_OP_WRITE
    LPVOID	   m_pData;				//操作的数据
    UINT	   m_nLen;				//操作的数据长度
    DWORD      m_dwWrite;           //写入字节数
    DWORD	   m_dwTimestamp;		//起始操作的时间戳
};

//IOCP线程池回调函数,实际就是完成通知响应函数
VOID CALLBACK IoCompletionCallback(PTP_CALLBACK_INSTANCE Instance,PVOID Context,PVOID Overlapped
                                   ,ULONG IoResult,ULONG_PTR NumberOfBytesTransferred,PTP_IO Io);
//写文件的线程
DWORD WINAPI WThread(LPVOID lpParameter);

//当前操作的文件对象的指针
LARGE_INTEGER g_liFilePointer = {};

//IOCP线程池
PTP_IO g_pThreadpoolIO = NULL;

int _tmain()
{
    GRS_USEPRINTF();

    TCHAR pFileName[MAX_PATH] = {};
    GetAppPath(pFileName);
    StringCchCat(pFileName,MAX_PATH,_T("NewIOCPFile.txt"));

    HANDLE ahWThread[GRS_MAXWTHREAD] = {};
    DWORD dwWrited = 0;

    //创建文件
    HANDLE hTxtFile = CreateFile(pFileName
        ,GENERIC_WRITE,0,NULL,CREATE_ALWAYS,FILE_ATTRIBUTE_NORMAL|FILE_FLAG_OVERLAPPED,NULL);
    if(INVALID_HANDLE_VALUE == hTxtFile)
    {
        GRS_PRINTF(_T("CreateFile(%s)失败,错误码:0x%08x\n")
            ,pFileName,GetLastError());
        _tsystem(_T("PAUSE"));
        return 0;
    }

    //初始化线程池环境数据结构
    TP_CALLBACK_ENVIRON PoolEnv = {};
    InitializeThreadpoolEnvironment(&PoolEnv);

    //创建IOCP线程池
    g_pThreadpoolIO = CreateThreadpoolIo(hTxtFile,(PTP_WIN32_IO_CALLBACK)IoCompletionCallback,
        hTxtFile,&PoolEnv);

    //启动IOCP线程池
    StartThreadpoolIo(g_pThreadpoolIO);

    //写入UNICODE文件的前缀码,以便正确打开
    ST_MY_OVERLAPPED* pMyOl = (ST_MY_OVERLAPPED*)GRS_CALLOC(sizeof(ST_MY_OVERLAPPED));
    GRS_ASSERT(NULL != pMyOl);

    pMyOl->m_dwOp = GRS_OP_WRITE;
    pMyOl->m_hFile = hTxtFile;
    pMyOl->m_pData = GRS_CALLOC(sizeof(WORD));
    GRS_ASSERT(NULL != pMyOl->m_pData);

    *((WORD*)pMyOl->m_pData) = MAKEWORD(0xff,0xfe);//UNICODE文本文件需要的前缀
    pMyOl->m_nLen = sizeof(WORD);

    //偏移文件指针
    pMyOl->m_ol.Offset = g_liFilePointer.LowPart;
    pMyOl->m_ol.OffsetHigh = g_liFilePointer.HighPart;
    g_liFilePointer.QuadPart += pMyOl->m_nLen;

    pMyOl->m_dwTimestamp = GetTickCount();//记录时间戳

    WriteFile((HANDLE)hTxtFile,pMyOl->m_pData,pMyOl->m_nLen,&pMyOl->m_dwWrite,(LPOVERLAPPED)&pMyOl->m_ol);

    //等待IOCP线程池完成操作
    WaitForThreadpoolIoCallbacks(g_pThreadpoolIO,FALSE);

    //启动写入线程进行日志写入操作
    for(int i = 0; i < GRS_MAXWTHREAD; i ++)
    {
        ahWThread[i] = GRS_BEGINTHREAD(WThread,hTxtFile);
    }

    //让主线等待这些写入线程结束
    WaitForMultipleObjects(GRS_MAXWTHREAD,ahWThread,TRUE,INFINITE);

    for(int i = 0; i < GRS_MAXWTHREAD; i ++)
    {
        CloseHandle(ahWThread[i]);
    }


    //关闭IOCP线程池
    CloseThreadpoolIo(g_pThreadpoolIO);

    //关闭日志文件
    if(INVALID_HANDLE_VALUE != hTxtFile )
    {
        CloseHandle(hTxtFile);
        hTxtFile = INVALID_HANDLE_VALUE;
    }


    _tsystem(_T("PAUSE"));
    return 0;
}

VOID CALLBACK IoCompletionCallback(PTP_CALLBACK_INSTANCE Instance,PVOID Context,PVOID Overlapped
                                   ,ULONG IoResult,ULONG_PTR NumberOfBytesTransferred,PTP_IO Io)
{
    GRS_USEPRINTF();
    if(NO_ERROR != IoResult)
    {
        GRS_PRINTF(_T("I/O操作出错,错误码:%u\n"),IoResult);
        return;
    }

    ST_MY_OVERLAPPED* pMyOl = CONTAINING_RECORD((LPOVERLAPPED)Overlapped,ST_MY_OVERLAPPED,m_ol);
    DWORD dwCurTimestamp = GetTickCount();

    switch(pMyOl->m_dwOp)
    {
    case GRS_OP_WRITE:
        {//写入操作结束
            GRS_PRINTF(_T("线程[0x%x]得到IO完成通知,完成操作(%s),缓冲(0x%08x)长度(%ubytes),写入时间戳(%u)当前时间戳(%u)时差(%u)\n"),
                GetCurrentThreadId(),GRS_OP_WRITE == pMyOl->m_dwOp?_T("Write"):_T("Read"),
                pMyOl->m_pData,pMyOl->m_nLen,pMyOl->m_dwTimestamp,dwCurTimestamp,dwCurTimestamp - pMyOl->m_dwTimestamp);

            GRS_SAFEFREE(pMyOl->m_pData);
            GRS_SAFEFREE(pMyOl);
        }
        break;
    case GRS_OP_READ:
        {//读取操作结束
        }
        break;
    default:
        {
        }
        break;
    }
}


#define MAX_LOGLEN 256

DWORD WINAPI WThread(LPVOID lpParameter)
{
    GRS_USEPRINTF();

    TCHAR pTxtContext[MAX_LOGLEN] = {};
    ST_MY_OVERLAPPED* pMyOl = NULL;
    size_t szLen = 0;
    LPTSTR pWriteText = NULL;

    StringCchPrintf(pTxtContext,MAX_LOGLEN,_T("这是一条模拟的日志记录,由线程[0x%x]写入\r\n")
        ,GetCurrentThreadId());
    StringCchLength(pTxtContext,MAX_LOGLEN,&szLen);

    szLen += 1;
    int i = 0;
    for(;i < GRS_MAXWRITEPERTHREAD; i ++)
    {
        pWriteText = (LPTSTR)GRS_CALLOC(szLen * sizeof(TCHAR));
        GRS_ASSERT(NULL != pWriteText);
        StringCchCopy(pWriteText,szLen,pTxtContext);

        //为每个操作申请一个自定义OL结构体,实际应用中这里考虑使用内存池
        pMyOl = (ST_MY_OVERLAPPED*)GRS_CALLOC(sizeof(ST_MY_OVERLAPPED));
        GRS_ASSERT(NULL != pMyOl);
        pMyOl->m_dwOp	= GRS_OP_WRITE;
        pMyOl->m_hFile	= (HANDLE)lpParameter;
        pMyOl->m_pData	= pWriteText;
        pMyOl->m_nLen	= szLen * sizeof(TCHAR);

        //这里使用原子操作同步文件指针,写入不会相互覆盖
        //这个地方体现了lock-free算法的精髓,使用了基本的CAS操作控制文件指针
        //比传统的使用关键代码段并等待的方法,这里用的方法要轻巧的多,付出的代价也小
        *((LONGLONG*)&pMyOl->m_ol.Pointer) = InterlockedCompareExchange64(&g_liFilePointer.QuadPart,
            g_liFilePointer.QuadPart + pMyOl->m_nLen,g_liFilePointer.QuadPart);

        pMyOl->m_dwTimestamp = GetTickCount();//记录时间戳

        StartThreadpoolIo(g_pThreadpoolIO);
        //写入
        WriteFile((HANDLE)lpParameter,pMyOl->m_pData,pMyOl->m_nLen,&pMyOl->m_dwWrite,(LPOVERLAPPED)&pMyOl->m_ol);
        if( ERROR_IO_PENDING != GetLastError() )
        {
            CancelThreadpoolIo(g_pThreadpoolIO);
        }
    }

    return i;
}