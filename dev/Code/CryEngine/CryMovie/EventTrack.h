/*
* All or portions of this file Copyright (c) Amazon.com, Inc. or its affiliates or
* its licensors.
*
* For complete copyright and license terms please see the LICENSE at the root of this
* distribution (the "License"). All use of this software is governed by the License,
* or, if provided, by the license below or the license accompanying this file. Do not
* remove or modify any license notices. This file is distributed on an "AS IS" BASIS,
* WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
*
*/
// Original file Copyright Crytek GMBH or its affiliates, used under license.

#ifndef CRYINCLUDE_CRYMOVIE_EVENTTRACK_H
#define CRYINCLUDE_CRYMOVIE_EVENTTRACK_H

#pragma once


//forward declarations.
#include "IMovieSystem.h"
#include "AnimTrack.h"
#include "AnimKey.h"

/** EntityTrack contains entity keys, when time reach event key, it fires script event or start animation etc...
*/
class CEventTrack
    : public TAnimTrack<IEventKey>
{
public:
    explicit CEventTrack(IAnimStringTable* pStrings);

    //////////////////////////////////////////////////////////////////////////
    // Overrides of IAnimTrack.
    //////////////////////////////////////////////////////////////////////////
    void GetKeyInfo(int key, const char*& description, float& duration);
    void SerializeKey(IEventKey& key, XmlNodeRef& keyNode, bool bLoading);
    void SetKey(int index, IKey* key);

    virtual void GetMemoryUsage(ICrySizer* pSizer) const
    {
        pSizer->AddObject(this, sizeof(*this));
    }

private:
    _smart_ptr<IAnimStringTable> m_pStrings;
};

#endif // CRYINCLUDE_CRYMOVIE_EVENTTRACK_H
