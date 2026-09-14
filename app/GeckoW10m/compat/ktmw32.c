/* ktmw32.c
 *
 * A stand-in for ktmw32.dll, which Windows 10 Mobile does not carry.
 *
 * xul.dll imports exactly two symbols from it, CreateTransaction and
 * CommitTransaction, and from exactly one place: the desktop toast
 * notification COM server registration in widget/windows/ToastNotification.cpp,
 * which wraps its registry writes in a transacted-NTFS transaction so a partial
 * registration rolls back. A packaged app registers no such COM server -- it
 * has the WinRT notification API instead, and its registry writes are
 * virtualised anyway -- so that code path is dead weight here. Without this
 * stub, though, the missing import stops the whole 132 MB engine from loading.
 *
 * The caller already handles failure: it checks the handle against
 * INVALID_HANDLE_VALUE and gives up cleanly, so refusing to start a
 * transaction sends Gecko down a path it was written to take.
 */
#include <windows.h>

HANDLE WINAPI CreateTransaction(LPSECURITY_ATTRIBUTES lpTransactionAttributes,
                                LPGUID UOW, DWORD CreateOptions,
                                DWORD IsolationLevel, DWORD IsolationFlags,
                                DWORD Timeout, LPWSTR Description) {
  (void)lpTransactionAttributes;
  (void)UOW;
  (void)CreateOptions;
  (void)IsolationLevel;
  (void)IsolationFlags;
  (void)Timeout;
  (void)Description;
  /* ERROR_RM_NOT_ACTIVE is what a real KTM reports when no resource manager is
     available, which is the truth here. */
  SetLastError(ERROR_RM_NOT_ACTIVE);
  return INVALID_HANDLE_VALUE;
}

BOOL WINAPI CommitTransaction(HANDLE TransactionHandle) {
  (void)TransactionHandle;
  SetLastError(ERROR_RM_NOT_ACTIVE);
  return FALSE;
}

BOOL WINAPI DllMain(HINSTANCE inst, DWORD reason, LPVOID reserved) {
  (void)reserved;
  if (reason == DLL_PROCESS_ATTACH) DisableThreadLibraryCalls(inst);
  return TRUE;
}
