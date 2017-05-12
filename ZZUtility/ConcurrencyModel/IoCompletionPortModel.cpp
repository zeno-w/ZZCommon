
#include <assert.h>
#include <ZZUtility/Synchronization/EventSync.h>
#include <ZZUtility/Synchronization/MutexSync.h>
#include "IoCompletionHandlerAbstract.h"
#include "IoCompletionPortModel.h"
#include "IoCompletionPortPrivateTypes.h"


// 并发控制处理线程
DWORD WINAPI IocpConcurrentProc(LPVOID lpParameter);

CIoCompletionPortModel *CIoCompletionPortModel::s_pSinglton = NULL;

CIoCompletionPortModel::CIoCompletionPortModel(void) : m(new CIoCompletionPortModelPrivate())
{
    SYSTEM_INFO sysInfo;
    GetSystemInfo(&sysInfo);
    m->vtrThreadHandle.reserve((sysInfo.dwNumberOfProcessors<<1) + 2); // 完成端口的最大并发数： CPU数量*2+2；
}

CIoCompletionPortModel::CIoCompletionPortModel(const CIoCompletionPortModel&) : 
	m(new CIoCompletionPortModelPrivate())
{}

CIoCompletionPortModel::~CIoCompletionPortModel(void)
{
    CloseHandle(m->hIocp);

    m->attchedSetAndThreadSync.Enter();
    std::vector<HANDLE>  vtrThreadHandle = m->vtrThreadHandle;
    m->attchedSetAndThreadSync.Leave();
    WaitForMultipleObjects(vtrThreadHandle.size(), &vtrThreadHandle[0],TRUE,INFINITE);
    delete m;
}

BOOL CIoCompletionPortModel::AttachHandler(CIoCompletionHandlerAbstract *pHandler)
{
	if (pHandler == NULL || 
		pHandler->m_hHandle == NULL || 
		pHandler->m_hHandle == INVALID_HANDLE_VALUE)
	{
		return FALSE;
	}

    m->attchedSetAndThreadSync.Enter();
    if (m->setIocpAttchedObj.find(pHandler) != m->setIocpAttchedObj.end())
    {
        m->attchedSetAndThreadSync.Leave();
        return TRUE;
    }
    
	HANDLE hIocp = CreateIoCompletionPort(pHandler->m_hHandle, m->hIocp, 
		reinterpret_cast<ULONG_PTR>(pHandler), m->vtrThreadHandle.capacity());
   if (hIocp == NULL)
   {
       pHandler->Destroy();
       m->attchedSetAndThreadSync.Leave();
       return FALSE;
   }

   pHandler->_AttachIocpModel();
   if (!pHandler->OverlapForIOCompletion())
   {
	   pHandler->Destroy();
	   m->attchedSetAndThreadSync.Leave();
	   return FALSE;
   }
   else
   {
	   _InterlockedIncrement(&pHandler->m->lOverlappedCount);
   }

   m->hIocp = hIocp;
   if (m->vtrThreadHandle.capacity() > m->vtrThreadHandle.size())
   {
       ThreadData *pThreadData = new ThreadData;
       pThreadData->pIocpModel = this;
       pThreadData->hThread = CreateThread(NULL,0,IocpConcurrentProc,pThreadData,0,NULL);
       if (NULL != pThreadData->hThread)
       {
           m->vtrThreadHandle.push_back(pThreadData->hThread);
       }
       else
       {
           delete pThreadData;
       }
   }

   if (m->vtrThreadHandle.empty())
   {
       m->attchedSetAndThreadSync.Leave();
       pHandler->_DetachIocpModel();
       CloseHandle(m->hIocp);
       m->hIocp = NULL;
       pHandler->Destroy();
       return FALSE;
   }

   m->setIocpAttchedObj.insert(pHandler);
   m->attchedSetAndThreadSync.Leave();


   return TRUE;
}

void CIoCompletionPortModel::DetachHandler(CIoCompletionHandlerAbstract *pHandler)
{
    m->attchedSetAndThreadSync.Enter();
    if (m->setIocpAttchedObj.find(pHandler) == m->setIocpAttchedObj.end())
    {
        m->attchedSetAndThreadSync.Leave();
        return;
    }

    m->setIocpAttchedObj.erase(pHandler);
    m->attchedSetAndThreadSync.Leave();

    if (pHandler->IsDestroyed())
    {
        pHandler->_DetachIocpModel();
    }
    else
    {
        pHandler->Destroy();
    }
}

BOOL CIoCompletionPortModel::PostHandlerStatus(CIoCompletionHandlerAbstract *pHandler)
{
	_InterlockedIncrement(&pHandler->m->lOverlappedCount);
    return ::PostQueuedCompletionStatus(m->hIocp,0,reinterpret_cast<ULONG_PTR>(pHandler),NULL);
}

BOOL CIoCompletionPortModel::_IsExitProcessThread(CIoCompletionHandlerAbstract *pCompletionHandler)
{
	if (pCompletionHandler->m->lOverlappedCount == 0)
	{
		pCompletionHandler->Destroy();

		m->attchedSetAndThreadSync.Enter();
		m->setIocpAttchedObj.erase(pCompletionHandler);
		BOOL bExit = m->setIocpAttchedObj.size() < m->vtrThreadHandle.size(); // 关联对象数量小于线程数时退出线程
		m->attchedSetAndThreadSync.Leave();

		if (pCompletionHandler->_IsAutoDelete())
		{
			pCompletionHandler->_DetachIocpModel();
			delete pCompletionHandler;
		}
		else
		{
			pCompletionHandler->_DetachIocpModel();
		}

		return bExit;
	}

	return FALSE;
}

void CIoCompletionPortModel::_IocpProc()
{
	DWORD           dwNumOfTransferredBytes;
	ULONG_PTR       lCompletionKey;
	LPOVERLAPPED    lpOverlapped;

	do
	{
		if (GetQueuedCompletionStatus(m->hIocp,&dwNumOfTransferredBytes,
			&lCompletionKey,&lpOverlapped,INFINITE))
		{
			if (lCompletionKey == NULL)
			{
				break;  // exit loop of do{...}while(lCompletionKey != NULL).
			}

			CIoCompletionHandlerAbstract *pIocpHandler = reinterpret_cast<CIoCompletionHandlerAbstract *>(lCompletionKey);
			if (pIocpHandler->DataTransferTrigger(dwNumOfTransferredBytes))
			{
				pIocpHandler->OverlapForIOCompletion();
				// _InterlockedIncrement(&pIocpHandler->m->lOverlappedCount);
			}
			else
			{
				_InterlockedDecrement(&pIocpHandler->m->lOverlappedCount);
			}

// 			switch (pIocpHandler->m_iIoCompletionStatus)
// 			{
// 			case ICS_OVERLAP_HANDLE:
// 				pIocpHandler->m_iIoCompletionStatus = ICS_LAST;
// 				if(pIocpHandler->OverlapForIOCompletion())
// 				{
// 					_InterlockedIncrement(&pIocpHandler->m->lOverlappedCount);
// 				}
// 				break;
// 			default:
// 				if (pIocpHandler->DataTransferTrigger(dwNumOfTransferredBytes))
// 				{
// 					_InterlockedIncrement(&pIocpHandler->m->lOverlappedCount);
// 				}
// 				break;
// 			}

			if (_IsExitProcessThread(pIocpHandler))
			{
				break;  // exit loop of do{...}while(lCompletionKey != NULL).
			}
		}
		else
		{
			if (lCompletionKey != NULL)
			{
				// 已重叠的句柄在直接关闭或者通讯另一端关闭退出时，会到达这一步骤。
				CIoCompletionHandlerAbstract *pIocpHandler = 
					reinterpret_cast<CIoCompletionHandlerAbstract*>(lCompletionKey);
				_InterlockedDecrement(&pIocpHandler->m->lOverlappedCount);
				pIocpHandler->HandleRaiseError(GetLastError());

				if (_IsExitProcessThread(pIocpHandler))
				{
					break;  // exit loop of do{...}while(lCompletionKey != NULL).
				}
			}
			else
			{
				// 完成端口句柄出现异常时。
				int iErrorCode = GetLastError();

				// TODO: prompt error info.
				break; // exit : while (lCompletionKey != NULL) loop!
			}
		}

	} while (lCompletionKey != NULL);
}

CIoCompletionPortModel * CIoCompletionPortModel::Instance()
{
    if (NULL == s_pSinglton)
    {
        // 单例模式支持线程安全的处理。
        CMutexSync mutex(TRUE,L"Local\\CIoCompletionPortModel");
        if (NULL == s_pSinglton)
        {
            CIoCompletionPortModel *pSinglton = new CIoCompletionPortModel();
            s_pSinglton = pSinglton;
        }
    }

    return s_pSinglton;
}

void CIoCompletionPortModel::Uninstance()
{
    CIoCompletionPortModel *pObject = s_pSinglton;
    s_pSinglton = NULL;
    delete pObject;
}

HANDLE CIoCompletionPortModel::GetHandle()
{
	return m->hIocp;
}

void CIoCompletionPortModel::_ThreadHanleProc(HANDLE hThread)
{
	_IocpProc();

	m->attchedSetAndThreadSync.Enter();
	for (auto itrBegin = m->vtrThreadHandle.begin(),	itrEnd = m->vtrThreadHandle.end();
		itrBegin != itrEnd; ++itrBegin)
	{
		if(hThread == *itrBegin)
		{
			HANDLE hThread = GetCurrentThread();
			m->vtrThreadHandle.erase(itrBegin);
			CloseHandle(hThread);
			break; // exit for;
		}
	}
	m->attchedSetAndThreadSync.Leave();
}

DWORD WINAPI IocpConcurrentProc(LPVOID lpParameter)
{
	CIoCompletionPortModel::ThreadData *pThreadData = 
		static_cast<CIoCompletionPortModel::ThreadData*>(lpParameter);

	pThreadData->pIocpModel->_ThreadHanleProc(pThreadData->hThread);

	delete pThreadData;

    return 0;
}
