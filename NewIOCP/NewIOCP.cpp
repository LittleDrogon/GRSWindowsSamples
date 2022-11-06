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

#define GRS_MAXWRITEPERTHREAD 100 //ÿ���߳����д�����
#define GRS_MAXWTHREAD		  20  //д���߳�����

#define GRS_OP_READ		0x1		//��ȡ����
#define GRS_OP_WRITE	0x2		//д�����

struct ST_MY_OVERLAPPED
{
    OVERLAPPED m_ol;				//Overlapped �ṹ,��һ���ǵ��ǵ�һ����Ա
    HANDLE	   m_hFile;				//�������ļ����
    DWORD	   m_dwOp;				//��������GRS_OP_READ/GRS_OP_WRITE
    LPVOID	   m_pData;				//����������
    UINT	   m_nLen;				//���������ݳ���
    DWORD      m_dwWrite;           //д���ֽ���
    DWORD	   m_dwTimestamp;		//��ʼ������ʱ���
};

//IOCP�̳߳ػص�����,ʵ�ʾ������֪ͨ��Ӧ����
VOID CALLBACK IoCompletionCallback(PTP_CALLBACK_INSTANCE Instance,PVOID Context,PVOID Overlapped
                                   ,ULONG IoResult,ULONG_PTR NumberOfBytesTransferred,PTP_IO Io);
//д�ļ����߳�
DWORD WINAPI WThread(LPVOID lpParameter);

//��ǰ�������ļ������ָ��
LARGE_INTEGER g_liFilePointer = {};

//IOCP�̳߳�
PTP_IO g_pThreadpoolIO = NULL;

int _tmain()
{
    GRS_USEPRINTF();

    TCHAR pFileName[MAX_PATH] = {};
    GetAppPath(pFileName);
    StringCchCat(pFileName,MAX_PATH,_T("NewIOCPFile.txt"));

    HANDLE ahWThread[GRS_MAXWTHREAD] = {};
    DWORD dwWrited = 0;

    //�����ļ�
    HANDLE hTxtFile = CreateFile(pFileName
        ,GENERIC_WRITE,0,NULL,CREATE_ALWAYS,FILE_ATTRIBUTE_NORMAL|FILE_FLAG_OVERLAPPED,NULL);
    if(INVALID_HANDLE_VALUE == hTxtFile)
    {
        GRS_PRINTF(_T("CreateFile(%s)ʧ��,������:0x%08x\n")
            ,pFileName,GetLastError());
        _tsystem(_T("PAUSE"));
        return 0;
    }

    //��ʼ���̳߳ػ������ݽṹ
    TP_CALLBACK_ENVIRON PoolEnv = {};
    InitializeThreadpoolEnvironment(&PoolEnv);

    //����IOCP�̳߳�
    g_pThreadpoolIO = CreateThreadpoolIo(hTxtFile,(PTP_WIN32_IO_CALLBACK)IoCompletionCallback,
        hTxtFile,&PoolEnv);

    //����IOCP�̳߳�
    StartThreadpoolIo(g_pThreadpoolIO);

    //д��UNICODE�ļ���ǰ׺��,�Ա���ȷ��
    ST_MY_OVERLAPPED* pMyOl = (ST_MY_OVERLAPPED*)GRS_CALLOC(sizeof(ST_MY_OVERLAPPED));
    GRS_ASSERT(NULL != pMyOl);

    pMyOl->m_dwOp = GRS_OP_WRITE;
    pMyOl->m_hFile = hTxtFile;
    pMyOl->m_pData = GRS_CALLOC(sizeof(WORD));
    GRS_ASSERT(NULL != pMyOl->m_pData);

    *((WORD*)pMyOl->m_pData) = MAKEWORD(0xff,0xfe);//UNICODE�ı��ļ���Ҫ��ǰ׺
    pMyOl->m_nLen = sizeof(WORD);

    //ƫ���ļ�ָ��
    pMyOl->m_ol.Offset = g_liFilePointer.LowPart;
    pMyOl->m_ol.OffsetHigh = g_liFilePointer.HighPart;
    g_liFilePointer.QuadPart += pMyOl->m_nLen;

    pMyOl->m_dwTimestamp = GetTickCount();//��¼ʱ���

    WriteFile((HANDLE)hTxtFile,pMyOl->m_pData,pMyOl->m_nLen,&pMyOl->m_dwWrite,(LPOVERLAPPED)&pMyOl->m_ol);

    //�ȴ�IOCP�̳߳���ɲ���
    WaitForThreadpoolIoCallbacks(g_pThreadpoolIO,FALSE);

    //����д���߳̽�����־д�����
    for(int i = 0; i < GRS_MAXWTHREAD; i ++)
    {
        ahWThread[i] = GRS_BEGINTHREAD(WThread,hTxtFile);
    }

    //�����ߵȴ���Щд���߳̽���
    WaitForMultipleObjects(GRS_MAXWTHREAD,ahWThread,TRUE,INFINITE);

    for(int i = 0; i < GRS_MAXWTHREAD; i ++)
    {
        CloseHandle(ahWThread[i]);
    }


    //�ر�IOCP�̳߳�
    CloseThreadpoolIo(g_pThreadpoolIO);

    //�ر���־�ļ�
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
        GRS_PRINTF(_T("I/O��������,������:%u\n"),IoResult);
        return;
    }

    ST_MY_OVERLAPPED* pMyOl = CONTAINING_RECORD((LPOVERLAPPED)Overlapped,ST_MY_OVERLAPPED,m_ol);
    DWORD dwCurTimestamp = GetTickCount();

    switch(pMyOl->m_dwOp)
    {
    case GRS_OP_WRITE:
        {//д���������
            GRS_PRINTF(_T("�߳�[0x%x]�õ�IO���֪ͨ,��ɲ���(%s),����(0x%08x)����(%ubytes),д��ʱ���(%u)��ǰʱ���(%u)ʱ��(%u)\n"),
                GetCurrentThreadId(),GRS_OP_WRITE == pMyOl->m_dwOp?_T("Write"):_T("Read"),
                pMyOl->m_pData,pMyOl->m_nLen,pMyOl->m_dwTimestamp,dwCurTimestamp,dwCurTimestamp - pMyOl->m_dwTimestamp);

            GRS_SAFEFREE(pMyOl->m_pData);
            GRS_SAFEFREE(pMyOl);
        }
        break;
    case GRS_OP_READ:
        {//��ȡ��������
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

    StringCchPrintf(pTxtContext,MAX_LOGLEN,_T("����һ��ģ�����־��¼,���߳�[0x%x]д��\r\n")
        ,GetCurrentThreadId());
    StringCchLength(pTxtContext,MAX_LOGLEN,&szLen);

    szLen += 1;
    int i = 0;
    for(;i < GRS_MAXWRITEPERTHREAD; i ++)
    {
        pWriteText = (LPTSTR)GRS_CALLOC(szLen * sizeof(TCHAR));
        GRS_ASSERT(NULL != pWriteText);
        StringCchCopy(pWriteText,szLen,pTxtContext);

        //Ϊÿ����������һ���Զ���OL�ṹ��,ʵ��Ӧ�������￼��ʹ���ڴ��
        pMyOl = (ST_MY_OVERLAPPED*)GRS_CALLOC(sizeof(ST_MY_OVERLAPPED));
        GRS_ASSERT(NULL != pMyOl);
        pMyOl->m_dwOp	= GRS_OP_WRITE;
        pMyOl->m_hFile	= (HANDLE)lpParameter;
        pMyOl->m_pData	= pWriteText;
        pMyOl->m_nLen	= szLen * sizeof(TCHAR);

        //����ʹ��ԭ�Ӳ���ͬ���ļ�ָ��,д�벻���໥����
        //����ط�������lock-free�㷨�ľ���,ʹ���˻�����CAS���������ļ�ָ��
        //�ȴ�ͳ��ʹ�ùؼ�����β��ȴ��ķ���,�����õķ���Ҫ���ɵĶ�,�����Ĵ���ҲС
        *((LONGLONG*)&pMyOl->m_ol.Pointer) = InterlockedCompareExchange64(&g_liFilePointer.QuadPart,
            g_liFilePointer.QuadPart + pMyOl->m_nLen,g_liFilePointer.QuadPart);

        pMyOl->m_dwTimestamp = GetTickCount();//��¼ʱ���

        StartThreadpoolIo(g_pThreadpoolIO);
        //д��
        WriteFile((HANDLE)lpParameter,pMyOl->m_pData,pMyOl->m_nLen,&pMyOl->m_dwWrite,(LPOVERLAPPED)&pMyOl->m_ol);
        if( ERROR_IO_PENDING != GetLastError() )
        {
            CancelThreadpoolIo(g_pThreadpoolIO);
        }
    }

    return i;
}