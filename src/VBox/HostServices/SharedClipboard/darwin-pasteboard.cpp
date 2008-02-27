/* $Id$ */
/** @file
 * Shared Clipboard: Mac OS X host implementation.
 */

/*
 * Copyright (C) 2008 innotek GmbH
 *
 * This file is part of VirtualBox Open Source Edition (OSE), as
 * available from http://www.virtualbox.org. This file is free software;
 * you can redistribute it and/or modify it under the terms of the GNU
 * General Public License (GPL) as published by the Free Software
 * Foundation, in version 2 as it comes in the "COPYING" file of the
 * VirtualBox OSE distribution. VirtualBox OSE is distributed in the
 * hope that it will be useful, but WITHOUT ANY WARRANTY of any kind.
 */

/// @todo: same as defined in VBoxClipboardSvc.h
/// @todo r-bird: why don't you include it?
#define VBOX_SHARED_CLIPBOARD_FMT_UNICODETEXT 0x01
#define VBOX_SHARED_CLIPBOARD_FMT_BITMAP      0x02
#define VBOX_SHARED_CLIPBOARD_FMT_HTML        0x04

#include <Carbon/Carbon.h>

#include <iprt/mem.h>
#include <iprt/assert.h>
#include "iprt/err.h"

#define LOG_GROUP LOG_GROUP_HGCM
#include "VBox/log.h"
#include "clipboard-helper.h"

//#define SHOW_CLIPBOARD_CONTENT

/** @todo r=bird: document these functions */

int initPasteboard (PasteboardRef *pPasteboardRef)
{
    int rc = VINF_SUCCESS;

    if (PasteboardCreate (kPasteboardClipboard, pPasteboardRef))
        rc = VERR_NOT_SUPPORTED;

    return rc;
}

void destroyPasteboard (PasteboardRef *pPasteboardRef)
{
    CFRelease (*pPasteboardRef);
    *pPasteboardRef = NULL;
}

int queryPasteboardFormats (PasteboardRef pPasteboard, uint32_t *pfFormats)
{
    Log (("queryPasteboardFormats\n"));

    OSStatus err = noErr;

    PasteboardSyncFlags syncFlags;
    /* Make sure all is in sync */
    syncFlags = PasteboardSynchronize (pPasteboard);
    /* If nothing changed return */
    if (!(syncFlags & kPasteboardModified))
        return VINF_SUCCESS;

    /* Are some items in the pasteboard? */
    ItemCount itemCount;
    err = PasteboardGetItemCount (pPasteboard, &itemCount);
    if (itemCount < 1)
        return VINF_SUCCESS;

    /* The id of the first element in the pastboard */
    int rc = VERR_NOT_SUPPORTED;
    PasteboardItemID itemID;
    if (!(err = PasteboardGetItemIdentifier (pPasteboard, 1, &itemID)))
    {
        /* Retrieve all flavors in the pasteboard, maybe there
         * is something we can use. */
        CFArrayRef flavorTypeArray;
        if (!(err = PasteboardCopyItemFlavors (pPasteboard, itemID, &flavorTypeArray)))
        {
            CFIndex flavorCount;
            flavorCount = CFArrayGetCount (flavorTypeArray);
            for (CFIndex flavorIndex = 0; flavorIndex < flavorCount; flavorIndex++)
            {
                CFStringRef flavorType;
                flavorType = static_cast <CFStringRef> (CFArrayGetValueAtIndex (flavorTypeArray,
                                                                                flavorIndex));
                /* Currently only unicode supported */
                if (UTTypeConformsTo (flavorType, CFSTR ("public.utf8-plain-text")) ||
                    UTTypeConformsTo (flavorType, CFSTR ("public.utf16-plain-text")))
                {
                    Log (("Unicode flavor detected.\n"));
                    *pfFormats |= VBOX_SHARED_CLIPBOARD_FMT_UNICODETEXT;
                }
            }
            rc = VINF_SUCCESS;
            CFRelease (flavorTypeArray);
        }
    }

    Log (("queryPasteboardFormats: rc = %02X\n", rc));
    return rc;
}

int readFromPasteboard (PasteboardRef pPasteboard, uint32_t fFormat, void *pv, uint32_t cb, uint32_t *pcbActual)
{
    Log (("readFromPastboard: fFormat = %02X\n", fFormat));

    OSStatus err = noErr;

    /* Make sure all is in sync */
    PasteboardSynchronize (pPasteboard);

    /* Are some items in the pasteboard? */
    ItemCount itemCount;
    err = PasteboardGetItemCount (pPasteboard, &itemCount);
    if (itemCount < 1)
        return VINF_SUCCESS;

    /* The id of the first element in the pastboard */
    int rc = VERR_NOT_SUPPORTED;
    PasteboardItemID itemID;
    if (!(err = PasteboardGetItemIdentifier (pPasteboard, 1, &itemID)))
    {
        /* The guest request unicode */
        if (fFormat & VBOX_SHARED_CLIPBOARD_FMT_UNICODETEXT)
        {
            CFDataRef outData;
            PRTUTF16 pwszTmp = NULL;
            /* Utf-16 is currently broken on more than one line.
             * Has to be investigated. */
#if 0
            /* Try utf-16 first */
            if (!(err = PasteboardCopyItemFlavorData (pPasteboard, itemID, CFSTR ("public.utf16-plain-text"), &outData)))
            {
                Log (("Clipboard content is utf-16\n"));
                rc = RTUtf16DupEx (&pwszTmp, (PRTUTF16)CFDataGetBytePtr (outData), 0);
            }
            /* Second try is utf-8 */
            else
#endif
                if (!(err = PasteboardCopyItemFlavorData (pPasteboard, itemID, CFSTR ("public.utf8-plain-text"), &outData)))
                {
                    Log (("readFromPastboard: clipboard content is utf-8\n"));
                    rc = RTStrToUtf16 ((const char*)CFDataGetBytePtr (outData), &pwszTmp);
                }
            if (pwszTmp)
            {
                /* Check how much longer will the converted text will be. */
                size_t cwSrc = RTUtf16Len (pwszTmp);
                size_t cwDest;
                rc = vboxClipboardUtf16GetWinSize (pwszTmp, cwSrc, &cwDest);
                if (RT_FAILURE (rc))
                {
                    RTUtf16Free (pwszTmp);
                    Log (("readFromPastboard: clipboard conversion failed.  vboxClipboardUtf16GetWinSize returned %Vrc.  Abandoning.\n", rc));
                    AssertRCReturn (rc, rc);
                }
                /* Set the actually needed data size */
                *pcbActual = cwDest * 2;
                /* Return success state */
                rc = VINF_SUCCESS;
                /* Do not copy data if the dst buffer is not big enough. */
                if (*pcbActual <= cb)
                {
                    rc = vboxClipboardUtf16LinToWin (pwszTmp, RTUtf16Len (pwszTmp), static_cast <PRTUTF16> (pv), cb / 2);
                    if (RT_FAILURE (rc))
                    {
                        RTUtf16Free (pwszTmp);
                        Log (("readFromPastboard: clipboard conversion failed.  vboxClipboardUtf16LinToWin() returned %Vrc.  Abandoning.\n", rc));
                        AssertRCReturn (rc, rc);
                    }
#ifdef SHOW_CLIPBOARD_CONTENT
                    Log (("readFromPastboard: clipboard content: %ls\n", static_cast <PRTUTF16> (pv)));
#endif
                }
                /* Free the temp string */
                RTUtf16Free (pwszTmp);
            }
        }
    }

    Log (("readFromPastboard: rc = %02X\n", rc));
    return rc;
}

int writeToPasteboard (PasteboardRef pPasteboard, void *pv, uint32_t cb, uint32_t fFormat)
{
    Log (("writeToPasteboard: fFormat = %02X\n", fFormat));

    /* Clear the pastboard */
    if (PasteboardClear (pPasteboard))
        return VERR_NOT_SUPPORTED;

    /* Make sure all is in sync */
    PasteboardSynchronize (pPasteboard);

    int rc = VERR_NOT_SUPPORTED;
    /* Handle the unicode text */
    if (fFormat & VBOX_SHARED_CLIPBOARD_FMT_UNICODETEXT)
    {
        PRTUTF16 pwszSrcText = static_cast <PRTUTF16> (pv);
        size_t cwSrc = cb / 2;
        size_t cwDest = 0;
        /* How long will the converted text be? */
        rc = vboxClipboardUtf16GetLinSize (pwszSrcText, cwSrc, &cwDest);
        if (RT_FAILURE (rc))
        {
            Log (("writeToPasteboard: clipboard conversion failed.  vboxClipboardUtf16GetLinSize returned %Vrc.  Abandoning.\n", rc));
            AssertRCReturn (rc, rc);
        }
        /* Empty clipboard? Not critical */
        if (cwDest == 0)
        {
            Log (("writeToPasteboard: received empty clipboard data from the guest, returning false.\n"));
            return VINF_SUCCESS;
        }
        /* Allocate the necessary memory */
        PRTUTF16 pwszDestText = static_cast <PRTUTF16> (RTMemAlloc (cwDest * 2));
        if (pwszDestText == NULL)
        {
            Log (("writeToPasteboard: failed to allocate %d bytes\n", cwDest * 2));
            return VERR_NO_MEMORY;
        }
        /* Convert the EOL */
        rc = vboxClipboardUtf16WinToLin (pwszSrcText, cwSrc, pwszDestText, cwDest);
        if (RT_FAILURE (rc))
        {
            Log (("writeToPasteboard: clipboard conversion failed.  vboxClipboardUtf16WinToLin() returned %Vrc.  Abandoning.\n", rc));
            RTMemFree (pwszDestText);
            AssertRCReturn (rc, rc);
        }

        CFDataRef textData = NULL;
        /* Item id is 1. Nothing special here. */
        PasteboardItemID itemId = (PasteboardItemID)1;
        /* Create a CData object which we could pass to the pasteboard */
        if ((textData = CFDataCreate (kCFAllocatorDefault,
                                      reinterpret_cast<UInt8*> (pwszDestText), cwDest * 2)))
        {
            /* Put the Utf-16 version to the pasteboard */
            PasteboardPutItemFlavor (pPasteboard, itemId,
                                     CFSTR ("public.utf16-plain-text"),
                                     textData, 0);
        }
        /* Create a Utf-8 version */
        char *pszDestText;
        rc = RTUtf16ToUtf8 (pwszDestText, &pszDestText);
        if (RT_SUCCESS (rc))
        {
            /* Create a CData object which we could pass to the pasteboard */
            if ((textData = CFDataCreate (kCFAllocatorDefault,
                                          reinterpret_cast<UInt8*> (pszDestText), RTUtf16CalcUtf8Len (pwszDestText)))) /** @todo r=bird: why not strlen(pszDestText)? */
            {
                /* Put the Utf-8 version to the pasteboard */
                PasteboardPutItemFlavor (pPasteboard, itemId,
                                         CFSTR ("public.utf8-plain-text"),
                                         textData, 0);
            }
            RTStrFree (pszDestText);
        }

        RTMemFree (pwszDestText);
        rc = VINF_SUCCESS;
    }
    else
        rc = VERR_NOT_IMPLEMENTED;

    Log (("writeToPasteboard: rc = %02X\n", rc));
    return rc;
}
