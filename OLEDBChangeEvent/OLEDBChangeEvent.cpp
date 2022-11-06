#include <tchar.h>
#include <windows.h>
#include <strsafe.h>

#define COM_NO_WINDOWS_H    //如果已经包含了Windows.h或不使用其他Windows库函数时
#define DBINITCONSTANTS
#define INITGUID
#define OLEDBVER 0x0260		//为了ICommandStream接口定义为2.6版
#include <oledb.h>
#include <oledberr.h>
#include <msdasc.h>

#define GRS_ALLOC(sz)		HeapAlloc(GetProcessHeap(),0,sz)
#define GRS_CALLOC(sz)		HeapAlloc(GetProcessHeap(),HEAP_ZERO_MEMORY,sz)
#define GRS_SAFEFREE(p)		if(NULL != p){HeapFree(GetProcessHeap(),0,p);p=NULL;}

#define GRS_USEPRINTF() TCHAR pBuf[1024] = {}
#define GRS_PRINTF(...) \
    StringCchPrintf(pBuf,1024,__VA_ARGS__);\
    WriteConsole(GetStdHandle(STD_OUTPUT_HANDLE),pBuf,lstrlen(pBuf),NULL,NULL);

#define GRS_SAFERELEASE(I) if(NULL != I){I->Release();I=NULL;}
#define GRS_COM_CHECK(hr,...) if(FAILED(hr)){GRS_PRINTF(__VA_ARGS__);goto CLEAR_UP;}


#define GRS_DEF_INTERFACE(i) i* p##i = NULL;
#define GRS_QUERYINTERFACE(i,iid) \
    if(FAILED(hr = i->QueryInterface(IID_##iid, (void **)&p##iid)) )\
    {\
    GRS_PRINTF(_T("不支持'%s'接口\n"),_T(#iid));\
    }\
    else\
    {\
    GRS_PRINTF(_T("支持'%s'接口\n"),_T(#iid));\
    }\
    GRS_SAFERELEASE(p##iid);

#define GRS_ROUNDUP_AMOUNT 8 
#define GRS_ROUNDUP_(size,amount) (((ULONG)(size)+((amount)-1))&~((amount)-1)) 
#define GRS_ROUNDUP(size) GRS_ROUNDUP_(size, GRS_ROUNDUP_AMOUNT) 

#define MAX_DISPLAY_SIZE	30
#define MIN_DISPLAY_SIZE    3

//工具宏 用于将通知事件的原因和步骤数字值转为字符串
#define GRS_VAL2WSTR_LEN    40
#define GRS_VAL2WSTR(val,cst,psz) if((val) == (cst)){StringCchCopy(psz,GRS_VAL2WSTR_LEN,_T(#cst));}

BOOL CreateDBSession(IOpenRowset*&pIOpenRowset);
VOID DisplayColumnNames(DBORDINAL        ulRowsetCols
                        ,DBCOLUMNINFO*   pColInfo
                        ,ULONG*          rgDispSize );
HRESULT DisplayRow(DBCOUNTITEM iRow,
                   DBCOUNTITEM cBindings,
                   DBBINDING * pChangeBindings,
                   void * pChangeData,
                   ULONG * rgDispSize );

VOID UpdateDisplaySize (DBCOUNTITEM   cBindings,
                        DBBINDING *   pChangeBindings,
                        void *        pChangeData,
                        ULONG *       rgDispSize );

VOID GetDBEventPhaseName(DBEVENTPHASE dbPhase,LPWSTR pszPhaseName);
VOID GetDBReasonName(DBREASON dbPhase,LPWSTR pszReasonName);

class CGRSRowsetNotify:
    public IRowsetNotify,		//Rowset Notifications
    public IDBAsynchNotify,		//Asynch Notifications
    public IRowPositionChange	//RowPosition Notifications
{
public:
    CGRSRowsetNotify()
    {
    }
    virtual ~CGRSRowsetNotify()
    {
    }
public:
    virtual HRESULT			Advise(IUnknown* pIUnknown,REFIID riid);
    virtual HRESULT			Unadvise(IUnknown* pIUnknown,REFIID riid);
protected:
    virtual HRESULT			Advise(IConnectionPoint* pIConnectionPoint,REFIID riid);
    virtual HRESULT			Unadvise(IConnectionPoint* pIConnectionPoint);
public:
    STDMETHODIMP_(ULONG)	AddRef(void);
    STDMETHODIMP_(ULONG)	Release(void);
    STDMETHODIMP			QueryInterface(REFIID riid, LPVOID *ppv);
public://行集变更通知回调接口
    //IRowsetNotify
    STDMETHODIMP OnFieldChange
        ( 
        IRowset* pIRowset,
        HROW hRow,
        DBORDINAL cColumns,
        DBORDINAL rgColumns[  ],
        DBREASON eReason,
        DBEVENTPHASE ePhase,
        BOOL fCantDeny
        );

    STDMETHODIMP OnRowChange
        ( 
        IRowset* pIRowset,
        DBCOUNTITEM cRows,
        const HROW rghRows[  ],
        DBREASON eReason,
        DBEVENTPHASE ePhase,
        BOOL fCantDeny
        );

    STDMETHODIMP OnRowsetChange
        ( 
        IRowset* pIRowset,
        DBREASON eReason,
        DBEVENTPHASE ePhase,
        BOOL fCantDeny
        );

public://以下接口用于异步数据库操作方式的回调接口实现
    //IDBAsynchNotify
    STDMETHODIMP OnLowResource
        ( 
        DB_DWRESERVE dwReserved
        );

    STDMETHODIMP OnProgress
        ( 
        HCHAPTER		hChapter,
        DBASYNCHOP		eOperation,
        DBCOUNTITEM		ulProgress,
        DBCOUNTITEM		ulProgressMax,
        DBASYNCHPHASE	eAsynchPhase,
        LPOLESTR pwszStatusText
        );

    STDMETHODIMP OnStop
        ( 
        HCHAPTER		hChapter,
        DBASYNCHOP		eOperation,
        HRESULT			hrStatus,
        LPOLESTR		pwszStatusText
        );
public://以下接口用于改变行位置之后的回调接口
    //IRowPositionChange
    STDMETHODIMP OnRowPositionChange
        (
        DBREASON			eReason,
        DBEVENTPHASE		ePhase,
        BOOL				fCantDeny
        );
protected:
    //Data
    ULONG			m_cRef;			// reference count
    DWORD           m_dwCookie;     // Cookie
};

HRESULT CGRSRowsetNotify::Advise(IUnknown* pIUnknown,REFIID riid)
{
    GRS_USEPRINTF();
    IConnectionPointContainer* pICpc = NULL;
    IConnectionPoint* pICPoint = NULL;
    HRESULT hr = pIUnknown->QueryInterface(IID_IConnectionPointContainer,(void**)&pICpc);
    if(FAILED(hr))
    {
        GRS_PRINTF(_T("通过IRowset接口获取IConnectionPointContainer接口失败,错误码:0x%08X\n"),hr);
        return hr;
    }
    hr = pICpc->FindConnectionPoint(riid,&pICPoint);
    if(FAILED(hr))
    {
        GRS_PRINTF(_T("获取IConnectionPoint接口失败,错误码:0x%08X\n"),hr);
        GRS_SAFERELEASE(pICpc);
        return hr;
    }
    GRS_SAFERELEASE(pICpc);

    hr = Advise(pICPoint,riid);
    GRS_SAFERELEASE(pICPoint);
    return hr;
}   
HRESULT CGRSRowsetNotify::Advise(IConnectionPoint* pIConnectionPoint,REFIID riid)
{
    GRS_USEPRINTF();
    if(NULL == pIConnectionPoint)
    {
        return E_INVALIDARG;
    }

    HRESULT hr = S_FALSE;
    
    if(IID_IRowsetNotify == riid)
    {
        hr = pIConnectionPoint->Advise(dynamic_cast<IRowsetNotify*>(this), &m_dwCookie);
    }
    if(IID_IDBAsynchNotify == riid)
    {
        hr = pIConnectionPoint->Advise(dynamic_cast<IDBAsynchNotify*>(this), &m_dwCookie);
    }
    if(IID_IRowPositionChange == riid)
    {
        hr = pIConnectionPoint->Advise(dynamic_cast<IRowPositionChange*>(this), &m_dwCookie);
    }

    if(FAILED(hr))
    {
        GRS_PRINTF(_T("IConnectionPoint::Advise方法调用失败,错误码:0x%08X\n"),hr);
        return hr;
    }

    return S_OK;
}

HRESULT CGRSRowsetNotify::Unadvise(IUnknown* pIUnknown,REFIID riid)
{
    GRS_USEPRINTF();
    IConnectionPointContainer* pICpc = NULL;
    IConnectionPoint* pICPoint = NULL;
    HRESULT hr = pIUnknown->QueryInterface(IID_IConnectionPointContainer,(void**)&pICpc);
    if(FAILED(hr))
    {
        GRS_PRINTF(_T("通过IRowset接口获取IConnectionPointContainer接口失败,错误码:0x%08X\n"),hr);
        return hr;
    }
    hr = pICpc->FindConnectionPoint(riid,&pICPoint);
    if(FAILED(hr))
    {
        GRS_PRINTF(_T("获取IConnectionPoint接口失败,错误码:0x%08X\n"),hr);
        GRS_SAFERELEASE(pICpc);
        return hr;
    }
    GRS_SAFERELEASE(pICpc);

    hr = Unadvise(pICPoint);
    GRS_SAFERELEASE(pICPoint);
    return hr;
}
HRESULT CGRSRowsetNotify::Unadvise(IConnectionPoint* pIConnectionPoint)
{
    GRS_USEPRINTF();
    if(NULL == pIConnectionPoint)
    {
        return E_INVALIDARG;
    }
    HRESULT hr = pIConnectionPoint->Unadvise(m_dwCookie);
    if(FAILED(hr))
    {
        GRS_PRINTF(_T("IConnectionPoint::Unadvise方法调用失败,错误码:0x%08X\n"),hr);
        return hr;
    }
    return S_OK;
}

STDMETHODIMP_(ULONG)	CGRSRowsetNotify::AddRef()
{
    return ++m_cRef;
}

STDMETHODIMP_(ULONG)	CGRSRowsetNotify::Release()
{
    if(--m_cRef)
        return m_cRef;
    delete this;
    return 0;
}

STDMETHODIMP CGRSRowsetNotify::QueryInterface(REFIID riid, LPVOID* ppv)
{
    if(!ppv)
    {
        return E_INVALIDARG;
    }

    *ppv = NULL;

    if(riid == IID_IUnknown)
    {
        *ppv = (IUnknown*)(IRowsetNotify*)this;
    }
    else if(riid == IID_IRowsetNotify)
    {
        *ppv = (IRowsetNotify*)this;
    }
    else if(riid == IID_IDBAsynchNotify)
    {
        *ppv = (IDBAsynchNotify*)this;
    }
    else if(riid == IID_IRowPositionChange)
    {
        *ppv = (IRowPositionChange*)this;
    }

    if(*ppv)
    {
        ((IUnknown*)*ppv)->AddRef();
        return S_OK;
    }	
    else
    {
        return E_NOINTERFACE;
    }
}

STDMETHODIMP CGRSRowsetNotify::OnFieldChange
( 
 /* [in] */ IRowset __RPC_FAR* pIRowset,
 /* [in] */ HROW hRow,
 /* [in] */ DBORDINAL cColumns,
 /* [size_is][in] */ DBORDINAL __RPC_FAR rgColumns[  ],
 /* [in] */ DBREASON eReason,
 /* [in] */ DBEVENTPHASE ePhase,
 /* [in] */ BOOL fCantDeny
 )
{
    GRS_USEPRINTF();
    WCHAR pszReason[GRS_VAL2WSTR_LEN] = {};
    WCHAR pszPhase[GRS_VAL2WSTR_LEN] = {};

    GetDBReasonName(eReason,(LPWSTR)pszReason);
    GetDBEventPhaseName(ePhase,(LPWSTR)pszPhase);

    GRS_PRINTF(_T("CGRSRowsetNotify::OnFieldChange[%lu Cols] Reason:%-40s Phase:%-40s\n")
        ,cColumns,pszReason,pszPhase);

    //if(DBREASON_COLUMN_SET == eReason && DBEVENTPHASE_OKTODO == ePhase)
    //{
    //    return S_FALSE;
    //}
    return S_OK;
}

STDMETHODIMP CGRSRowsetNotify::OnRowChange
( 
 /* [in] */ IRowset __RPC_FAR* pIRowset,
 /* [in] */ DBCOUNTITEM cRows,
 /* [size_is][in] */ const HROW __RPC_FAR rghRows[  ],
 /* [in] */ DBREASON eReason,
 /* [in] */ DBEVENTPHASE ePhase,
 /* [in] */ BOOL fCantDeny
 )
{
    GRS_USEPRINTF();
    WCHAR pszReason[GRS_VAL2WSTR_LEN] = {};
    WCHAR pszPhase[GRS_VAL2WSTR_LEN] = {};

    GetDBReasonName(eReason,(LPWSTR)pszReason);
    GetDBEventPhaseName(ePhase,(LPWSTR)pszPhase);

    GRS_PRINTF(_T("CGRSRowsetNotify::OnRowChange  [%lu Rows] Reason:%-40s Phase:%-40s\n")
        ,cRows,pszReason,pszPhase);
    return S_OK;
}

STDMETHODIMP CGRSRowsetNotify::OnRowsetChange
( 
 /* [in] */ IRowset __RPC_FAR* pIRowset,
 /* [in] */ DBREASON eReason,
 /* [in] */ DBEVENTPHASE ePhase,
 /* [in] */ BOOL fCantDeny)
{
    GRS_USEPRINTF();
    WCHAR pszReason[GRS_VAL2WSTR_LEN] = {};
    WCHAR pszPhase[GRS_VAL2WSTR_LEN] = {};

    GetDBReasonName(eReason,(LPWSTR)pszReason);
    GetDBEventPhaseName(ePhase,(LPWSTR)pszPhase);

    GRS_PRINTF(_T("CGRSRowsetNotify::OnRowsetChange,       Reason:%-40s Phase:%-40s\n")
        ,pszReason,pszPhase);
    return S_OK;
}

STDMETHODIMP CGRSRowsetNotify::OnLowResource(DB_DWRESERVE dwReserved)
{
    return S_OK;
}

STDMETHODIMP CGRSRowsetNotify::OnProgress
( 
 HCHAPTER		hChapter,
 DBASYNCHOP		eOperation,
 DBCOUNTITEM		ulProgress,
 DBCOUNTITEM		ulProgressMax,
 DBASYNCHPHASE	eAsynchPhase,
 LPOLESTR		pwszStatusText
 )
{
    //Output Notification
    //return TRACE_NOTIFICATION(NOTIFY_IDBASYNCHNOTIFY, m_hrReturn, L"IDBAsynchNotify", L"OnProgress", L"(0x%p, %s, %lu, %lu, %s, %s)", hChapter, GetAsynchReason(eOperation), ulProgress, ulProgressMax, GetAsynchPhase(eAsynchPhase), pwszStatusText);
    return S_OK;
}

STDMETHODIMP CGRSRowsetNotify::OnStop
( 
 HCHAPTER hChapter,
 ULONG    ulOperation,
 HRESULT  hrStatus,
 LPOLESTR pwszStatusText
 )
{
    //Output Notification
    //return TRACE_NOTIFICATION(NOTIFY_IDBASYNCHNOTIFY, m_hrReturn, L"IDBAsynchNotify", L"OnStop", L"(0x%p, %s, %S, %s)", hChapter, GetAsynchReason(ulOperation), GetErrorName(hrStatus), pwszStatusText);
    return S_OK;
}

STDMETHODIMP CGRSRowsetNotify::OnRowPositionChange(
    DBREASON		eReason,
    DBEVENTPHASE	ePhase,
    BOOL			fCantDeny)
{
    //Output Notification
    //return TRACE_NOTIFICATION(NOTIFY_IROWPOSITIONCHANGE, m_hrReturn, L"IRowPositionChange", L"OnRowPositionChange", L"(%s, %s, %s)", GetReasonName(eReason), GetPhaseName(ePhase), fCantDeny ? L"TRUE" : L"FALSE");
    return S_OK;
}

int _tmain()
{
    GRS_USEPRINTF();
    //CoInitializeEx(NULL,COINIT_MULTITHREADED);
    CoInitialize(NULL);

    HRESULT				hr					= S_OK;
    IOpenRowset*		pIOpenRowset		= NULL;
    IDBCreateCommand*	pIDBCreateCommand	= NULL;

    ICommand*			pICommand			= NULL;
    ICommandText*		pICommandText		= NULL;
    ICommandProperties* pICommandProperties = NULL;
    GRS_DEF_INTERFACE(IAccessor);
    GRS_DEF_INTERFACE(IColumnsInfo);
    GRS_DEF_INTERFACE(IRowset);
    GRS_DEF_INTERFACE(IRowsetChange);
    GRS_DEF_INTERFACE(IRowsetUpdate);

    TCHAR* pSQL = _T("Select top 10 * From T_State");
    DBPROPSET ps[1] = {};
    DBPROP prop[20]	= {};

    ULONG               ulRowsetCols		= 0;
    ULONG				ulRealCols			= 0;
    DBCOLUMNINFO*		pColInfo			= NULL;
    LPWSTR              pStringBuffer       = NULL;
    ULONG               iCol				= 0;
    ULONG               ulChangeOffset      = 0;
    ULONG				ulShowOffset		= 0;
    DBBINDING*			pChangeBindings     = NULL;// 用于数据更新的绑定
    DBBINDING*			pShowBindings		= NULL;// 用于数据显示的绑定
    DBROWSTATUS			parRowStatus[1]		= {};
    HACCESSOR			hChangeAccessor		= NULL;
    HACCESSOR			hShowAccessor		= NULL;
    void*				pChangeData         = NULL;
    void*				pCurChangeData		= NULL;
    void*				pShowData			= NULL;
    void*				pCurShowData		= NULL;
    ULONG               pRowsObtained		= 0;
    HROW *              phRows				= NULL;
    HROW				hNewRow				= NULL;
    ULONG               iRow				= 0;
    ULONG				ulAllRows			= 0;
    LONG                lReadRows           = 2;//一次读取2行
    DBLENGTH            ulLength			= 0;
    WCHAR*				pwsValue			= NULL;
    BYTE*				pbNewData			= NULL;
    ULONG*				plDisplayLen		= NULL;
    BOOL				bShowColumnName		= FALSE;
    DBROWSTATUS*        pUpdateStatus       = NULL;
    CGRSRowsetNotify    RowsetNotify;

    if(!CreateDBSession(pIOpenRowset))
    {
        _tsystem(_T("PAUSE"));
        return -1;
    }
    hr = pIOpenRowset->QueryInterface(IID_IDBCreateCommand,(void**)&pIDBCreateCommand);
    GRS_COM_CHECK(hr,_T("获取IDBCreateCommand接口失败，错误码：0x%08X\n"),hr);

    hr = pIDBCreateCommand->CreateCommand(NULL,IID_ICommand,(IUnknown**)&pICommand);
    GRS_COM_CHECK(hr,_T("创建ICommand接口失败，错误码：0x%08X\n"),hr);

    //执行一个命令
    hr = pICommand->QueryInterface(IID_ICommandText,(void**)&pICommandText);
    GRS_COM_CHECK(hr,_T("获取ICommandText接口失败，错误码：0x%08X\n"),hr);
    hr = pICommandText->SetCommandText(DBGUID_DEFAULT,pSQL);
    GRS_COM_CHECK(hr,_T("设置SQL语句失败，错误码：0x%08X\n"),hr);

    //设置属性
    prop[0].dwPropertyID    = DBPROP_UPDATABILITY;
    prop[0].vValue.vt       = VT_I4;
    //设置执行后的结果集可以进行增删改操作
    prop[0].vValue.lVal     = DBPROPVAL_UP_CHANGE     //打开Update属性
        |DBPROPVAL_UP_DELETE					//打开Delete属性
        |DBPROPVAL_UP_INSERT;					//打开Insert属性
    prop[0].dwOptions       = DBPROPOPTIONS_REQUIRED;
    prop[0].colid = DB_NULLID;

    //打开IRowsetUpdate接口也就是支持延迟的更新特性
    prop[1].dwPropertyID    = DBPROP_IRowsetUpdate;
    prop[1].vValue.vt       = VT_BOOL;
    prop[1].vValue.boolVal  = VARIANT_TRUE;         
    prop[1].dwOptions       = DBPROPOPTIONS_REQUIRED;
    prop[1].colid           = DB_NULLID;

    //设置最大可修改行数，这里根据需要设置为150行
    prop[2].dwPropertyID    = DBPROP_MAXPENDINGROWS;
    prop[2].vValue.vt       = VT_I4;
    prop[2].vValue.intVal   = 150;         
    prop[2].dwOptions       = DBPROPOPTIONS_REQUIRED;
    prop[2].colid           = DB_NULLID;

    //以下这个属性也是要紧句，不设置那么就不能即更新删除又同时插入新行
    prop[3].dwPropertyID    = DBPROP_CANHOLDROWS;
    prop[3].vValue.vt       = VT_BOOL;
    prop[3].vValue.boolVal  = VARIANT_TRUE;         
    prop[3].dwOptions       = DBPROPOPTIONS_REQUIRED;
    prop[3].colid           = DB_NULLID;

  
    prop[4].dwPropertyID    = DBPROP_IConnectionPointContainer;
    prop[4].vValue.vt       = VT_BOOL;
    prop[4].vValue.boolVal  = VARIANT_TRUE;         
    prop[4].dwOptions       = DBPROPOPTIONS_REQUIRED;
    prop[4].colid           = DB_NULLID;

    ps[0].guidPropertySet   = DBPROPSET_ROWSET;       //注意属性集合的名称
    ps[0].cProperties       = 5;							//属性集中属性个数
    ps[0].rgProperties      = prop;

    //设置属性 执行语句 获得行集
    hr = pICommandText->QueryInterface(IID_ICommandProperties,(void**)& pICommandProperties);
    GRS_COM_CHECK(hr,_T("获取ICommandProperties接口失败，错误码：0x%08X\n"),hr);

    hr = pICommandProperties->SetProperties(1,ps);//注意必须在Execute前设定属性
    GRS_COM_CHECK(hr,_T("设置Command对象属性失败，错误码：0x%08X\n"),hr);
    
    hr = pICommandText->Execute(NULL,IID_IRowsetChange,NULL,NULL,(IUnknown**)&pIRowsetChange);
    GRS_COM_CHECK(hr,_T("执行SQL语句失败，错误码：0x%08X\n"),hr);

    //获得IRowsetUpdate接口
    hr = pIRowsetChange->QueryInterface(IID_IRowsetUpdate,(void**)&pIRowsetUpdate);
    GRS_COM_CHECK(hr,_T("获取IRowsetUpdate接口失败，错误码：0x%08X\n"),hr);

    //准备绑定信息
    hr = pIRowsetChange->QueryInterface(IID_IColumnsInfo,(void**)&pIColumnsInfo);
    GRS_COM_CHECK(hr,_T("获得IColumnsInfo接口失败，错误码：0x%08X\n"),hr);
    
    hr = pIColumnsInfo->GetColumnInfo(&ulRowsetCols, &pColInfo,&pStringBuffer);
    GRS_COM_CHECK(hr,_T("获取列信息失败，错误码：0x%08X\n"),hr);

    //分配绑定结构数组的内存	
    pChangeBindings = (DBBINDING*)GRS_CALLOC(ulRowsetCols * sizeof(DBBINDING));
    pShowBindings	= (DBBINDING*)GRS_CALLOC(ulRowsetCols * sizeof(DBBINDING));

    if( 0 == pColInfo[0].iOrdinal )
    {//第0列 就不要绑定了 防止后面的插入不成功
        ulRealCols = ulRowsetCols - 1;
        for( iCol = 1; iCol < ulRowsetCols; iCol++ )
        {
            pChangeBindings[iCol - 1].iOrdinal   = pColInfo[iCol].iOrdinal;
            pChangeBindings[iCol - 1].dwPart     = DBPART_VALUE|DBPART_LENGTH|DBPART_STATUS;
            pChangeBindings[iCol - 1].obStatus   = ulChangeOffset;
            pChangeBindings[iCol - 1].obLength   = ulChangeOffset + sizeof(DBSTATUS);
            pChangeBindings[iCol - 1].obValue    = ulChangeOffset + sizeof(DBSTATUS) + sizeof(ULONG);
            pChangeBindings[iCol - 1].dwMemOwner = DBMEMOWNER_CLIENTOWNED;
            pChangeBindings[iCol - 1].eParamIO   = DBPARAMIO_NOTPARAM;
            pChangeBindings[iCol - 1].bPrecision = pColInfo[iCol].bPrecision;
            pChangeBindings[iCol - 1].bScale     = pColInfo[iCol].bScale;
            pChangeBindings[iCol - 1].wType      = DBTYPE_WSTR;//pColInfo[iCol].wType;	//强制转为WSTR
            pChangeBindings[iCol - 1].cbMaxLen   = pColInfo[iCol].ulColumnSize * sizeof(WCHAR);
            ulChangeOffset = pChangeBindings[iCol - 1].cbMaxLen + pChangeBindings[iCol - 1].obValue;
            ulChangeOffset = GRS_ROUNDUP(ulChangeOffset);
        }
    }
    else
    {
        ulRealCols = ulRowsetCols;
        //根据列信息 填充绑定结构信息数组 注意行大小进行8字节边界对齐排列
        for( iCol = 0; iCol < ulRowsetCols; iCol++ )
        {
            pChangeBindings[iCol].iOrdinal   = pColInfo[iCol].iOrdinal;
            pChangeBindings[iCol].dwPart     = DBPART_VALUE|DBPART_LENGTH|DBPART_STATUS;
            pChangeBindings[iCol].obStatus   = ulChangeOffset;
            pChangeBindings[iCol].obLength   = ulChangeOffset + sizeof(DBSTATUS);
            pChangeBindings[iCol].obValue    = ulChangeOffset + sizeof(DBSTATUS) + sizeof(ULONG);
            pChangeBindings[iCol].dwMemOwner = DBMEMOWNER_CLIENTOWNED;
            pChangeBindings[iCol].eParamIO   = DBPARAMIO_NOTPARAM;
            pChangeBindings[iCol].bPrecision = pColInfo[iCol].bPrecision;
            pChangeBindings[iCol].bScale     = pColInfo[iCol].bScale;
            pChangeBindings[iCol].wType      = DBTYPE_WSTR;//pColInfo[iCol].wType;	//强制转为WSTR
            pChangeBindings[iCol].cbMaxLen   = pColInfo[iCol].ulColumnSize * sizeof(WCHAR);
            ulChangeOffset = pChangeBindings[iCol].cbMaxLen + pChangeBindings[iCol].obValue;
            ulChangeOffset = GRS_ROUNDUP(ulChangeOffset);
        }

    }

    for( iCol = 0; iCol < ulRowsetCols; iCol++ )
    {
        pShowBindings[iCol].iOrdinal   = pColInfo[iCol].iOrdinal;
        pShowBindings[iCol].dwPart     = DBPART_VALUE | DBPART_LENGTH | DBPART_STATUS;
        pShowBindings[iCol].obStatus   = ulShowOffset;
        pShowBindings[iCol].obLength   = ulShowOffset + sizeof(DBSTATUS);
        pShowBindings[iCol].obValue    = ulShowOffset + sizeof(DBSTATUS) + sizeof(ULONG);
        pShowBindings[iCol].dwMemOwner = DBMEMOWNER_CLIENTOWNED;
        pShowBindings[iCol].eParamIO   = DBPARAMIO_NOTPARAM;
        pShowBindings[iCol].bPrecision = pColInfo[iCol].bPrecision;
        pShowBindings[iCol].bScale     = pColInfo[iCol].bScale;
        pShowBindings[iCol].wType      = DBTYPE_WSTR;//pColInfo[iCol].wType;	//强制转为WSTR
        pShowBindings[iCol].cbMaxLen   = pColInfo[iCol].ulColumnSize * sizeof(WCHAR);
        ulShowOffset = pShowBindings[iCol].cbMaxLen + pShowBindings[iCol].obValue;
        ulShowOffset = GRS_ROUNDUP(ulShowOffset);
    }

    //分配显示宽度数组
    plDisplayLen = (ULONG*)GRS_CALLOC(ulRowsetCols * sizeof(ULONG));

    //创建行访问器
    hr = pIRowsetChange->QueryInterface(IID_IAccessor,(void**)&pIAccessor);
    GRS_COM_CHECK(hr,_T("获得IAccessor接口失败，错误码：0x%08X\n"),hr);
    hr = pIAccessor->CreateAccessor( DBACCESSOR_ROWDATA,ulRowsetCols,pShowBindings,0,&hShowAccessor,NULL);
    GRS_COM_CHECK(hr,_T("创建行显示访问器失败，错误码：0x%08X\n"),hr);
    hr = pIAccessor->CreateAccessor( DBACCESSOR_ROWDATA,ulRealCols,pChangeBindings,0,&hChangeAccessor,NULL);
    GRS_COM_CHECK(hr,_T("创建行更新访问器失败，错误码：0x%08X\n"),hr);

    //获取数据
    hr = pIRowsetChange->QueryInterface(IID_IRowset,(void**)&pIRowset);
    GRS_COM_CHECK(hr,_T("获得IRowset接口失败，错误码：0x%08X\n"),hr);

    hr = RowsetNotify.Advise(pIRowset,IID_IRowsetNotify);
    GRS_COM_CHECK(hr,_T("添加IRowsetNotify回调接口失败，错误码：0x%08X\n"),hr);

    //分配lReadRows行数据的缓冲，然后反复读取lReadRows行到这里，然后逐行处理之
    pChangeData = GRS_CALLOC(ulChangeOffset * lReadRows);
    pShowData	= GRS_CALLOC(ulShowOffset * lReadRows);
    ulAllRows = 0;

    while( S_OK == pIRowset->GetNextRows(DB_NULL_HCHAPTER,0,lReadRows,&pRowsObtained, &phRows) )    
    {//循环读取数据，每次循环默认读取lReadRows行，实际读取到pRowsObtained行
        //每次新显示一批数据时都先清0一下宽度数组
        ZeroMemory(plDisplayLen,ulRowsetCols * sizeof(ULONG));
        for( iRow = 0; iRow < pRowsObtained; iRow++ )
        {
            //计算每列最大的显示宽度
            pCurChangeData    = (BYTE*)pChangeData + (ulChangeOffset * iRow);
            hr = pIRowset->GetData(phRows[iRow],hChangeAccessor,pCurChangeData);

            pCurShowData    = (BYTE*)pShowData + (ulShowOffset * iRow);
            hr = pIRowset->GetData(phRows[iRow],hShowAccessor,pCurShowData);

            UpdateDisplaySize(ulRowsetCols,pShowBindings,pCurShowData,plDisplayLen);
            //UpdateDisplaySize(ulRealCols,pChangeBindings,pCurChangeData,plDisplayLen);
        }

        if(!bShowColumnName)
        {//显示字段信息
            DisplayColumnNames(ulRowsetCols,pColInfo,plDisplayLen);
            //DisplayColumnNames(ulRealCols,&pColInfo[1],plDisplayLen);
            bShowColumnName = TRUE;
        }

        for( iRow = 0; iRow < pRowsObtained; iRow++ )
        {
            //根据偏移取得行数据
            pCurChangeData    = (BYTE*)pChangeData + (ulChangeOffset * iRow);
            pCurShowData    = (BYTE*)pShowData + (ulShowOffset * iRow);
            //显示数据
            DisplayRow(++ulAllRows,ulRowsetCols,pShowBindings,pCurShowData,plDisplayLen);
            //DisplayRow(++ulAllRows,ulRealCols,pChangeBindings,pCurChangeData,plDisplayLen);

            //取得数据指针 注意 取得是第二个字段，也就是名称字段进行修改
            //注意实际的数据长度也要修改为正确的值，单位是字节
            *(DBLENGTH *)((BYTE *)pCurChangeData + pChangeBindings[1].obLength) = 4;
            pwsValue    = (WCHAR*)((BYTE *)pCurChangeData + pChangeBindings[1].obValue);
            StringCchCopy(pwsValue,pChangeBindings[1].cbMaxLen/sizeof(WCHAR),_T("测试"));

            hr = pIRowsetChange->SetData(phRows[iRow],hChangeAccessor,pCurChangeData);
            if(FAILED(hr))
            {
                GRS_PRINTF(_T("调用SetData更新数据失败，错误码：0x%08X\n"),hr);
            }
            //GRS_COM_CHECK(hr,_T("调用SetData更新数据失败，错误码：0x%08X\n"),hr);
        }

        //删除一行数据
        hr = pIRowsetChange->DeleteRows(DB_NULL_HCHAPTER,1,&phRows[0],parRowStatus);
        GRS_COM_CHECK(hr,_T("调用DeleteRows删除行失败，错误码：0x%08X\n"),hr);


        if( pRowsObtained )
        {//释放行句柄数组
            pIRowset->ReleaseRows(pRowsObtained,phRows,NULL,NULL,NULL);
        }
        bShowColumnName = FALSE;
        CoTaskMemFree(phRows);
        phRows = NULL;
    }

    //未设置DBPROP_CANHOLDROWS属性为TRUE时,之前的修改要提交掉之后才能插入新行
    //hr = pIRowsetUpdate->Update(NULL,0,NULL,NULL,NULL,&pUpdateStatus);
    //GRS_COM_CHECK(hr,_T("调用Update提交更新失败，错误码：0x%08X\n"),hr);

    //分配一个新行数据 设置数据后 插入
    pbNewData = (BYTE*)GRS_CALLOC(ulChangeOffset);

    //设置第一个字段
    *(DBLENGTH *)((BYTE *)pbNewData + pChangeBindings[0].obLength) = 12;
    pwsValue = (WCHAR*) (pbNewData + pChangeBindings[0].obValue);
    StringCchCopy(pwsValue,pChangeBindings[1].cbMaxLen/sizeof(WCHAR),_T("000000"));

    //设置第二个字段
    *(DBLENGTH *)((BYTE *)pbNewData + pChangeBindings[1].obLength) = 4;
    pwsValue = (WCHAR*) (pbNewData + pChangeBindings[1].obValue);
    StringCchCopy(pwsValue,pChangeBindings[1].cbMaxLen/sizeof(WCHAR),_T("中国"));

    //插入新数据
    hr = pIRowsetChange->InsertRow(NULL,hChangeAccessor,pbNewData,&hNewRow);
    GRS_COM_CHECK(hr,_T("调用InsertRow插入新行失败，错误码：0x%08X\n"),hr);

    //最终提交所有的更新到数据库中
    hr = pIRowsetUpdate->Update(NULL,0,NULL,NULL,NULL,&pUpdateStatus);
    GRS_COM_CHECK(hr,_T("调用Update提交更新失败，错误码：0x%08X\n"),hr);

CLEAR_UP:
    if(NULL != pIAccessor)
    {
        if(hChangeAccessor)
        {
            pIAccessor->ReleaseAccessor(hChangeAccessor,NULL);
        }
        if(hShowAccessor)
        {
            pIAccessor->ReleaseAccessor(hShowAccessor,NULL);
        }
    }
    GRS_SAFEFREE(pChangeData);
    GRS_SAFEFREE(pChangeBindings);
    GRS_SAFEFREE(pShowData);
    GRS_SAFEFREE(pShowBindings);
    GRS_SAFEFREE(plDisplayLen);
    GRS_SAFEFREE(pbNewData);
    CoTaskMemFree(pColInfo);
    CoTaskMemFree(pStringBuffer);
    CoTaskMemFree(pUpdateStatus);
    GRS_SAFERELEASE(pIRowset);
    GRS_SAFERELEASE(pIAccessor);
    GRS_SAFERELEASE(pIColumnsInfo);
    GRS_SAFERELEASE(pIRowsetChange);
    GRS_SAFERELEASE(pIRowsetUpdate);
    GRS_SAFERELEASE(pICommandProperties);
    GRS_SAFERELEASE(pICommand);
    GRS_SAFERELEASE(pICommandText);
    GRS_SAFERELEASE(pIOpenRowset);
    GRS_SAFERELEASE(pIDBCreateCommand);

    _tsystem(_T("PAUSE"));
    CoUninitialize();
    return 0;
}

VOID GetDBEventPhaseName(DBEVENTPHASE dbPhase,LPWSTR pszPhaseName)
{
    GRS_VAL2WSTR(dbPhase,DBEVENTPHASE_OKTODO,pszPhaseName);
    GRS_VAL2WSTR(dbPhase,DBEVENTPHASE_ABOUTTODO,pszPhaseName);
    GRS_VAL2WSTR(dbPhase,DBEVENTPHASE_SYNCHAFTER,pszPhaseName);
    GRS_VAL2WSTR(dbPhase,DBEVENTPHASE_FAILEDTODO,pszPhaseName);
    GRS_VAL2WSTR(dbPhase,DBEVENTPHASE_DIDEVENT,pszPhaseName);
}
VOID GetDBReasonName(DBREASON dbReason,LPWSTR pszReasonName)
{
    GRS_VAL2WSTR(dbReason,DBREASON_ROW_ASYNCHINSERT,pszReasonName);
    GRS_VAL2WSTR(dbReason,DBREASON_ROWSET_FETCHPOSITIONCHANGE,pszReasonName);
    GRS_VAL2WSTR(dbReason,DBREASON_ROWSET_RELEASE,pszReasonName);
    GRS_VAL2WSTR(dbReason,DBREASON_ROWSET_CHANGED,pszReasonName);
    GRS_VAL2WSTR(dbReason,DBREASON_COLUMN_SET,pszReasonName);
    GRS_VAL2WSTR(dbReason,DBREASON_COLUMN_RECALCULATED,pszReasonName);
    GRS_VAL2WSTR(dbReason,DBREASON_ROW_ACTIVATE,pszReasonName);
    GRS_VAL2WSTR(dbReason,DBREASON_ROW_RELEASE,pszReasonName);
    GRS_VAL2WSTR(dbReason,DBREASON_ROW_DELETE,pszReasonName);
    GRS_VAL2WSTR(dbReason,DBREASON_ROW_FIRSTCHANGE,pszReasonName);
    GRS_VAL2WSTR(dbReason,DBREASON_ROW_INSERT,pszReasonName);
    GRS_VAL2WSTR(dbReason,DBREASON_ROW_RESYNCH,pszReasonName);
    GRS_VAL2WSTR(dbReason,DBREASON_ROW_UNDOCHANGE,pszReasonName);
    GRS_VAL2WSTR(dbReason,DBREASON_ROW_UNDOINSERT,pszReasonName);
    GRS_VAL2WSTR(dbReason,DBREASON_ROW_UNDODELETE,pszReasonName);
    GRS_VAL2WSTR(dbReason,DBREASON_ROW_UPDATE,pszReasonName);
}

BOOL CreateDBSession(IOpenRowset*&pIOpenRowset)
{
    GRS_USEPRINTF();
    GRS_SAFERELEASE(pIOpenRowset);

    IDBPromptInitialize*	pIDBPromptInitialize	= NULL;
    IDBInitialize*			pDBConnect				= NULL;
    IDBCreateSession*		pIDBCreateSession		= NULL;

    HWND hWndParent = GetDesktopWindow();
    CLSID clsid = {};

    //1、使用OLEDB数据库连接对话框连接到数据源
    HRESULT hr = CoCreateInstance(CLSID_DataLinks, NULL, CLSCTX_INPROC_SERVER,
        IID_IDBPromptInitialize, (void **)&pIDBPromptInitialize);
    if(FAILED(hr))
    {
        GRS_PRINTF(_T("无法创建IDBPromptInitialize接口，错误码：0x%08x\n"),hr);
        GRS_SAFERELEASE(pDBConnect);
        GRS_SAFERELEASE(pIDBPromptInitialize);
        return FALSE;
    }

    //下面这句将弹出数据库连接对话框
    hr = pIDBPromptInitialize->PromptDataSource(NULL, hWndParent,
        DBPROMPTOPTIONS_PROPERTYSHEET, 0, NULL, NULL, IID_IDBInitialize,
        (IUnknown **)&pDBConnect);
    if( FAILED(hr) )
    {
        GRS_PRINTF(_T("弹出数据库源对话框错误，错误码：0x%08x\n"),hr);
        GRS_SAFERELEASE(pDBConnect);
        GRS_SAFERELEASE(pIDBPromptInitialize);
        return FALSE;
    }
    GRS_SAFERELEASE(pIDBPromptInitialize);

    hr = pDBConnect->Initialize();//根据对话框采集的参数连接到指定的数据库
    if( FAILED(hr) )
    {
        GRS_PRINTF(_T("初始化数据源连接错误，错误码：0x%08x\n"),hr);
        GRS_SAFERELEASE(pDBConnect);
        return FALSE;
    }

    if(FAILED(hr = pDBConnect->QueryInterface(IID_IDBCreateSession,(void**)& pIDBCreateSession)))
    {
        GRS_PRINTF(_T("获取IDBCreateSession接口失败，错误码：0x%08x\n"),hr);
        GRS_SAFERELEASE(pIDBCreateSession);
        GRS_SAFERELEASE(pDBConnect);
        return FALSE;
    }
    GRS_SAFERELEASE(pDBConnect);

    hr = pIDBCreateSession->CreateSession(NULL,IID_IOpenRowset,(IUnknown**)&pIOpenRowset);
    if(FAILED(hr))
    {
        GRS_PRINTF(_T("创建Session对象获取IOpenRowset接口失败，错误码：0x%08x\n"),hr);
        GRS_SAFERELEASE(pDBConnect);
        GRS_SAFERELEASE(pIDBCreateSession);
        GRS_SAFERELEASE(pIOpenRowset);
        return FALSE;
    }
    //代表数据源连接的接口可以不要了，需要时可以通过Session对象的IGetDataSource接口获取
    GRS_SAFERELEASE(pDBConnect);
    GRS_SAFERELEASE(pIDBCreateSession);
    return TRUE;
}


VOID DisplayColumnNames(DBORDINAL        ulRowsetCols
                        ,DBCOLUMNINFO*   pColInfo
                        ,ULONG*          rgDispSize )
{
    GRS_USEPRINTF();
    WCHAR            wszColumn[MAX_DISPLAY_SIZE + 1];
    LPWSTR           pwszColName;
    DBORDINAL        iCol;
    ULONG            cSpaces;
    ULONG            iSpace;
    size_t			 szLen;

    // Display the title of the row index column.
    GRS_PRINTF(_T("   Row    | "));

    for( iCol = 0; iCol < ulRowsetCols; iCol++ ) 
    {
        pwszColName = pColInfo[iCol].pwszName;
        if( !pwszColName ) 
        {
            if( !pColInfo[iCol].iOrdinal )
            {
                pwszColName = _T("Bmk");
            }
            else
            {
                pwszColName = _T("(null)");
            }
        }

        StringCchCopy(wszColumn,MAX_DISPLAY_SIZE + 1, pwszColName);
        StringCchLength(wszColumn,MAX_DISPLAY_SIZE + 1,&szLen);
        rgDispSize[iCol] = max( min(rgDispSize[iCol], MAX_DISPLAY_SIZE + 1) , szLen);
        cSpaces = rgDispSize[iCol] - szLen;

        GRS_PRINTF(_T("%s"), wszColumn);
        for(iSpace = 0; iSpace < cSpaces; iSpace++ )
        {
            GRS_PRINTF(_T(" "));
        }
        if( iCol < ulRowsetCols - 1 )
        {
            GRS_PRINTF(_T(" | "));
        }
    }
    GRS_PRINTF(_T("\n"));
}

HRESULT DisplayRow(DBCOUNTITEM iRow,
                   DBCOUNTITEM cBindings,
                   DBBINDING * pChangeBindings,
                   void * pChangeData,
                   ULONG * rgDispSize )
{
    GRS_USEPRINTF();
    HRESULT               hr = S_OK;
    WCHAR                 wszColumn[MAX_DISPLAY_SIZE + 1];
    DBSTATUS              dwStatus;
    DBLENGTH              ulLength;
    void *                pwsValue;
    DBCOUNTITEM           iCol;
    ULONG                 cbRead;
    ISequentialStream *   pISeqStream = NULL;
    ULONG                 cSpaces;
    ULONG                 iSpace;
    size_t				  szLen;

    GRS_PRINTF(_T("[%8d]| "), iRow);
    for( iCol = 0; iCol < cBindings; iCol++ ) 
    {
        dwStatus   = *(DBSTATUS *)((BYTE *)pChangeData + pChangeBindings[iCol].obStatus);
        ulLength   = *(DBLENGTH *)((BYTE *)pChangeData + pChangeBindings[iCol].obLength);
        pwsValue    = (BYTE *)pChangeData + pChangeBindings[iCol].obValue;
        switch( dwStatus ) 
        {
        case DBSTATUS_S_ISNULL:
            StringCchCopy(wszColumn,MAX_DISPLAY_SIZE + 1, L"(null)");
            break;
        case DBSTATUS_S_TRUNCATED:
        case DBSTATUS_S_OK:
        case DBSTATUS_S_DEFAULT:
            {
                switch( pChangeBindings[iCol].wType )
                {
                case DBTYPE_WSTR:
                    {   
                        StringCchCopy(wszColumn,MAX_DISPLAY_SIZE + 1,(WCHAR*)pwsValue);
                        break;
                    }
                case DBTYPE_IUNKNOWN:
                    {
                        pISeqStream = *(ISequentialStream**)pwsValue;
                        hr = pISeqStream->Read(
                            wszColumn,                     
                            MAX_DISPLAY_SIZE,              
                            &cbRead                        
                            );

                        wszColumn[cbRead / sizeof(WCHAR)] = L'\0';
                        pISeqStream->Release();
                        pISeqStream = NULL;
                        break;
                    }
                }
                break;
            }
        default:
            StringCchCopy(wszColumn, MAX_DISPLAY_SIZE + 1, _T("(error status)"));
            break;
        }
        StringCchLength(wszColumn,MAX_DISPLAY_SIZE + 1,&szLen);
        cSpaces = min(rgDispSize[iCol], MAX_DISPLAY_SIZE + 1) - szLen;

        GRS_PRINTF(_T("%s"), wszColumn);

        for(iSpace = 0; iSpace < cSpaces; iSpace++ )
        {
            GRS_PRINTF(_T(" "));
        }


        if( iCol < cBindings - 1 )
        {
            GRS_PRINTF(_T(" | "));
        }
    }

    GRS_SAFERELEASE(pISeqStream )
        GRS_PRINTF(_T("\n"));
    return hr;
}

VOID UpdateDisplaySize (DBCOUNTITEM   cBindings,
                        DBBINDING *   pChangeBindings,
                        void *        pChangeData,
                        ULONG *       rgDispSize )
{
    DBSTATUS      dwStatus;
    ULONG         cchLength;
    DBCOUNTITEM   iCol;

    for( iCol = 0; iCol < cBindings; iCol++ )
    {
        dwStatus = *(DBSTATUS *)((BYTE *)pChangeData + pChangeBindings[iCol].obStatus);
        cchLength = ((*(ULONG *)((BYTE *)pChangeData + pChangeBindings[iCol].obLength))
            / sizeof(WCHAR));
        switch( dwStatus )
        {
        case DBSTATUS_S_ISNULL:
            cchLength = 6;                              // "(null)"
            break;

        case DBSTATUS_S_TRUNCATED:
        case DBSTATUS_S_OK:
        case DBSTATUS_S_DEFAULT:
            if( pChangeBindings[iCol].wType == DBTYPE_IUNKNOWN )
                cchLength = 2 + 8;                      // "0x%08lx"

            cchLength = max(cchLength, MIN_DISPLAY_SIZE);
            break;

        default:
            cchLength = 14;                        // "(error status)"
            break;
        }

        if( rgDispSize[iCol] < cchLength )
        {
            rgDispSize[iCol] = cchLength;
        }
    }
}