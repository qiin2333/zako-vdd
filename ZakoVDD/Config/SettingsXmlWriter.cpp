#include "DriverSettings.h"

#include "..\Core\DriverState.h"
#include "..\Logging\Logger.h"

#include <atlbase.h>
#include <mutex>
#include <shlwapi.h>
#include <string>
#include <xmllite.h>

using namespace std;

namespace
{
mutex s_xmlWriteMutex;

bool UpdateXmlElementText(const wchar_t *targetElement, const wchar_t *replacementText)
{
	lock_guard<mutex> xmlLock(s_xmlWriteMutex);

	const wstring settingsname = confpath + L"\\vdd_settings.xml";
	CComPtr<IStream> pFileStream;
	HRESULT hr = SHCreateStreamOnFileEx(settingsname.c_str(), STGM_READWRITE, FILE_ATTRIBUTE_NORMAL, FALSE, nullptr, &pFileStream);
	if (FAILED(hr))
	{
		VDD_LOG_ERROR("UpdatingXML: XML file could not be opened.");
		return false;
	}

	CComPtr<IXmlReader> pReader;
	hr = CreateXmlReader(__uuidof(IXmlReader), reinterpret_cast<void **>(&pReader), nullptr);
	if (FAILED(hr))
	{
		VDD_LOG_ERROR("UpdatingXML: Failed to create XML reader.");
		return false;
	}
	hr = pReader->SetInput(pFileStream);
	if (FAILED(hr))
	{
		VDD_LOG_ERROR("UpdatingXML: Failed to set XML reader input.");
		return false;
	}

	CComPtr<IStream> pOutFileStream;
	wstring tempFileName = settingsname + L".temp";
	hr = SHCreateStreamOnFileEx(tempFileName.c_str(), STGM_CREATE | STGM_WRITE, FILE_ATTRIBUTE_NORMAL, TRUE, nullptr, &pOutFileStream);
	if (FAILED(hr))
	{
		VDD_LOG_ERROR("UpdatingXML: Failed to create output file stream.");
		return false;
	}

	CComPtr<IXmlWriter> pWriter;
	hr = CreateXmlWriter(__uuidof(IXmlWriter), reinterpret_cast<void **>(&pWriter), nullptr);
	if (FAILED(hr))
	{
		VDD_LOG_ERROR("UpdatingXML: Failed to create XML writer.");
		return false;
	}
	hr = pWriter->SetOutput(pOutFileStream);
	if (FAILED(hr))
	{
		VDD_LOG_ERROR("UpdatingXML: Failed to set XML writer output.");
		return false;
	}
	hr = pWriter->WriteStartDocument(XmlStandalone_Omit);
	if (FAILED(hr))
	{
		VDD_LOG_ERROR("UpdatingXML: Failed to write start of the document.");
		return false;
	}

	XmlNodeType nodeType;
	const wchar_t *pwszLocalName = nullptr;
	const wchar_t *pwszValue = nullptr;
	bool targetElementFound = false;

	while (S_OK == pReader->Read(&nodeType))
	{
		switch (nodeType)
		{
		case XmlNodeType_Element:
			if (FAILED(pReader->GetLocalName(&pwszLocalName, nullptr)) || pwszLocalName == nullptr)
			{
				return false;
			}
			pWriter->WriteStartElement(nullptr, pwszLocalName, nullptr);
			targetElementFound = (wcscmp(pwszLocalName, targetElement) == 0);
			break;

		case XmlNodeType_EndElement:
			pWriter->WriteEndElement();
			targetElementFound = false;
			break;

		case XmlNodeType_Text:
			if (FAILED(pReader->GetValue(&pwszValue, nullptr)) || pwszValue == nullptr)
			{
				return false;
			}
			if (targetElementFound)
			{
				pWriter->WriteString(replacementText);
				targetElementFound = false;
			}
			else
			{
				pWriter->WriteString(pwszValue);
			}
			break;

		case XmlNodeType_Whitespace:
			if (FAILED(pReader->GetValue(&pwszValue, nullptr)) || pwszValue == nullptr)
			{
				return false;
			}
			pWriter->WriteWhitespace(pwszValue);
			break;

		case XmlNodeType_Comment:
			if (FAILED(pReader->GetValue(&pwszValue, nullptr)) || pwszValue == nullptr)
			{
				return false;
			}
			pWriter->WriteComment(pwszValue);
			break;
		}
	}

	hr = pWriter->WriteEndDocument();
	if (FAILED(hr))
	{
		return false;
	}

	pFileStream.Release();
	pOutFileStream.Release();
	pWriter.Release();
	pReader.Release();

	if (!MoveFileExW(tempFileName.c_str(), settingsname.c_str(), MOVEFILE_REPLACE_EXISTING))
	{
		return false;
	}

	return true;
}
}

bool UpdateXmlToggleSetting(bool toggle, const wchar_t *variable)
{
	return UpdateXmlElementText(variable, toggle ? L"true" : L"false");
}

bool UpdateXmlGpuSetting(const wchar_t *gpuName)
{
	return UpdateXmlElementText(L"gpu", gpuName);
}

bool UpdateXmlDisplayCountSetting(int displayCount)
{
	const wstring value = to_wstring(displayCount);
	return UpdateXmlElementText(L"count", value.c_str());
}
