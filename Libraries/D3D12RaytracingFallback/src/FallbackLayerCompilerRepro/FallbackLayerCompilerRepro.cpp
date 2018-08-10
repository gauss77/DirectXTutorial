//*********************************************************
//
// Copyright (c) Microsoft. All rights reserved.
// This code is licensed under the MIT License (MIT).
// THIS CODE IS PROVIDED *AS IS* WITHOUT WARRANTY OF
// ANY KIND, EITHER EXPRESS OR IMPLIED, INCLUDING ANY
// IMPLIED WARRANTIES OF FITNESS FOR A PARTICULAR
// PURPOSE, MERCHANTABILITY, OR NON-INFRINGEMENT.
//
//*********************************************************

#include "stdafx.h"
#include <fstream>

using namespace FallbackLayer;

HRESULT VerifyResult(IDxcOperationResult *pResult)
{
    HRESULT hr;

    if (pResult == NULL)
    {
        return -1;
    }

    pResult->GetStatus(&hr);
    if (FAILED(hr))
    {
        CComPtr<IDxcBlobEncoding> pErrorText;
        pResult->GetErrorBuffer(&pErrorText);
        fprintf(stderr, "Error: Failed to compile the shader:");
        OutputDebugStringA((char *)pErrorText->GetBufferPointer());
    }
}

int main(int argc, char **argv)
{
    if (argc == 1)
    {
        printf("Please provide command line args.\n");
        printf("Usage: [maxAttributeSizeInBytes] [libraries] [exportNames]\n");
        printf("[libraries] and [exportNames] are semicolon-separated strings.\n");
        return 0;
    }

    UINT maxAttributeSize = std::stoul(argv[1], nullptr, 0);

    std::vector<char *> libFileNames;
    std::vector<std::ifstream*> libFiles;
    std::vector<int> libFileSizes;

    char *libFileName;
    char *nextLibFileName;
    libFileName = strtok_s(argv[2], ";", &nextLibFileName);
    while (libFileName != NULL)
    {
        libFileNames.push_back(libFileName);
        std::ifstream libFile(libFileName, std::ios::ate | std::ios::binary);
        libFileSizes.push_back(static_cast<int>(libFile.tellg()));
        libFile.seekg(0);
        libFiles.push_back(&libFile);
        libFileName = strtok_s(NULL, ";", &nextLibFileName);
    }

    std::vector<LPCWSTR> exportNames;

    char *exportName;
    char *nextExportName;
    exportName = strtok_s(argv[3], ";", &nextExportName);
    while (exportName != NULL)
    {
        wchar_t *convertedExportName = new wchar_t[128];
        mbstowcs_s(NULL, convertedExportName, 128, exportName, 128);
        exportNames.push_back((LPCWSTR)convertedExportName);
        exportName = strtok_s(NULL, ";", &nextExportName);
    }

    printf("\n\n**** Reproducing call to DxilShaderPatcher::LinkCollection ****\n");
    printf("Max Attribute Size = %d\n", maxAttributeSize);
    printf("Libraries:\n");
    for (UINT i = 0; i < libFiles.size(); i++)
    {
        printf("\t%s [%d bytes]\n", libFileNames[i], libFileSizes[i]);
    }
    printf("Export names:\n");
    for (UINT i = 0; i < exportNames.size(); i++)
    {
        printf("\t%S\n", exportNames[i]);
    }

    std::vector<DxcShaderBytecode> pLibBlobPtrs(libFiles.size());
    for (UINT libIndex = 0; libIndex < libFiles.size(); libIndex++)
    {
        size_t libBytecodeLength = libFileSizes[libIndex];
        void *libByteCode = new char[libBytecodeLength];
        libFiles[libIndex]->read((char *)libByteCode, libBytecodeLength);

        pLibBlobPtrs[libIndex] = { (LPBYTE)libByteCode, (UINT32)libBytecodeLength };
    }

    dxc::DxcDllSupport dxcDxrFallbackSupport;
    ThrowFailure(dxcDxrFallbackSupport.InitializeForDll(L"DxrFallbackCompiler.dll", "DxcCreateDxrFallbackCompiler"),
        L"Failed to load DxrFallbackCompiler.dll, verify this executable is in the executable directory."
        L" The Fallback Layer is sensitive to the DxrFallbackCompiler.dll version, make sure the"
        L" DxrFallbackCompiler.dll is the correct version packaged with the Fallback");
    
    CComPtr<IDxcDxrFallbackCompiler> pFallbackCompiler;
    ThrowFailure(dxcDxrFallbackSupport.CreateInstance(CLSID_DxcDxrFallbackCompiler, &pFallbackCompiler),
        L"Failed to create an instance of the Fallback Compiler. This suggests the version of DxrFallbackCompiler.dll "
        L"is being used that doesn't match up with the Fallback layer. Verify that the DxrFallbackCompiler.dll is from "
        L"same package as the Fallback.");
    pFallbackCompiler->SetDebugOutput(3);

    CComPtr<IDxcBlob> pCollectionBlob;
    std::vector<DxcShaderInfo> shaderInfo(exportNames.size());
    CComPtr<IDxcOperationResult> pResult;
    
    printf("\n\n**** Calling Compile() ****\n\n");
    HRESULT compilerResult = pFallbackCompiler->Compile(pLibBlobPtrs.data(), (UINT32)pLibBlobPtrs.size(), exportNames.data(), shaderInfo.data(), (UINT32)exportNames.size(), maxAttributeSize, &pResult);

    for (UINT libIndex = 0; libIndex < libFiles.size(); libIndex++)
    {
        delete[] pLibBlobPtrs[libIndex].pData;
    }

    for (UINT exportNameIndex = 0; exportNameIndex < exportNames.size(); exportNameIndex++)
    {
        delete[] exportNames[exportNameIndex];
    }

    if (FAILED(compilerResult))
    {
        if (pResult == NULL)
        {
            fprintf(stderr, "Error: Compiler gave a null result.\n");
            return -1;
        }

        if (FAILED(VerifyResult(pResult)))
        {
            return -1;
        }

        if (FAILED(pResult->GetResult(&pCollectionBlob)))
        {
            fprintf(stderr, "Error: Failed to get result from compiler.\n");
            return -1;
        }
    }

    printf("Everything seems to be working!\n");

    return 0;
}

