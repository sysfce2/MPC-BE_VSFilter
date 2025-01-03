/*
 * (C) 2003-2006 Gabest
 * (C) 2006-2024 see Authors.txt
 *
 * This file is part of MPC-BE.
 *
 * MPC-BE is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 3 of the License, or
 * (at your option) any later version.
 *
 * MPC-BE is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 */

#include "stdafx.h"
#include <afxinet.h>
#include "TextFile.h"
#include <Utf8.h>
#include "DSUtil/FileHandle.h"
#include "DSUtil/HTTPAsync.h"

#define TEXTFILE_BUFFER_SIZE (64 * 1024)

CTextFile::CTextFile(enc encoding/* = ASCII*/, enc defaultencoding/* = ASCII*/)
	: m_encoding(encoding)
	, m_defaultencoding(defaultencoding)
	, m_offset(0)
	, m_posInFile(0)
	, m_posInBuffer(0)
	, m_nInBuffer(0)
{
	m_buffer.reset(new(std::nothrow) char[TEXTFILE_BUFFER_SIZE]);
	m_wbuffer.reset(new(std::nothrow) WCHAR[TEXTFILE_BUFFER_SIZE]);
}

CTextFile::~CTextFile()
{
	Close();
}

bool CTextFile::OpenFile(LPCWSTR lpszFileName, LPCWSTR mode)
{
	Close();

	FILE* f = nullptr;
	auto err = _wfopen_s(&f, lpszFileName, mode);
	if (err != 0 || !f) {
		return false;
	}

	m_pFile.reset(f);
	m_pStdioFile = std::make_unique<CStdioFile>(f);

	m_strFileName = lpszFileName;

	return true;
}

bool CTextFile::Open(LPCWSTR lpszFileName)
{
	if (!OpenFile(lpszFileName, L"rb")) {
		return false;
	}

	m_offset = 0;
	m_nInBuffer = m_posInBuffer = 0;

	if (m_pStdioFile->GetLength() >= 2) {
		WORD w;
		if (sizeof(w) != m_pStdioFile->Read(&w, sizeof(w))) {
			Close();
			return false;
		}

		if (w == 0xfeff) {
			m_encoding = LE16;
			m_offset = 2;
		} else if (w == 0xfffe) {
			m_encoding = BE16;
			m_offset = 2;
		} else if (w == 0xbbef && m_pStdioFile->GetLength() >= 3) {
			BYTE b;
			if (sizeof(b) != m_pStdioFile->Read(&b, sizeof(b))) {
				Close();
				return false;
			}

			if (b == 0xbf) {
				m_encoding = UTF8;
				m_offset = 3;
			}
		}
	}

	if (m_encoding == ASCII) {
		if (!ReopenAsText()) {
			return false;
		}
	} else if (m_offset == 0) { // No BOM detected, ensure the file is read from the beginning
		Seek(0, CStdioFile::begin);
	} else {
		m_posInFile = m_pStdioFile->GetPosition();
	}

	return true;
}

bool CTextFile::ReopenAsText()
{
	auto fileName = m_strFileName;

	Close();

	return OpenFile(fileName, L"rt");
}

bool CTextFile::Save(LPCWSTR lpszFileName, enc e)
{
	if (!OpenFile(lpszFileName, e == ASCII ? L"wt" : L"wb")) {
		return false;
	}

	if (e == UTF8) {
		BYTE b[3] = {0xef, 0xbb, 0xbf};
		m_pStdioFile->Write(b, sizeof(b));
	} else if (e == LE16) {
		BYTE b[2] = {0xff, 0xfe};
		m_pStdioFile->Write(b, sizeof(b));
	} else if (e == BE16) {
		BYTE b[2] = {0xfe, 0xff};
		m_pStdioFile->Write(b, sizeof(b));
	}

	m_encoding = e;

	return true;
}

void CTextFile::Close()
{
	if (m_pStdioFile) {
		m_pStdioFile.reset();
		m_pFile.reset();
		m_strFileName.Empty();
	}
}

void CTextFile::SetEncoding(enc e)
{
	m_encoding = e;
}

CTextFile::enc CTextFile::GetEncoding() const
{
	return m_encoding;
}

bool CTextFile::IsUnicode() const
{
	return m_encoding == UTF8 || m_encoding == LE16 || m_encoding == BE16;
}

CStringW CTextFile::GetFilePath() const
{
	return m_strFileName;
}

// CStdioFile

ULONGLONG CTextFile::GetPosition() const
{
	return m_pStdioFile ? (m_pStdioFile->GetPosition() - m_offset - (m_nInBuffer - m_posInBuffer)) : 0ULL;
}

ULONGLONG CTextFile::GetLength() const
{
	return m_pStdioFile ? (m_pStdioFile->GetLength() - m_offset) : 0ULL;
}

ULONGLONG CTextFile::Seek(LONGLONG lOff, UINT nFrom)
{
	if (!m_pStdioFile) {
		return 0ULL;
	}

	ULONGLONG newPos;

	// Try to reuse the buffer if any
	if (m_nInBuffer > 0) {
		const LONGLONG pos = GetPosition();
		const LONGLONG len = GetLength();

		switch (nFrom) {
			default:
			case CStdioFile::begin:
				break;
			case CStdioFile::current:
				lOff = pos + lOff;
				break;
			case CStdioFile::end:
				lOff = len - lOff;
				break;
		}

		lOff = std::clamp(lOff, 0LL, len);

		m_posInBuffer += lOff - pos;
		if (m_posInBuffer < 0 || m_posInBuffer >= m_nInBuffer) {
			// If we would have to end up out of the buffer, we just reset it and seek normally
			m_nInBuffer = m_posInBuffer = 0;
			newPos = m_pStdioFile->Seek(lOff + m_offset, CStdioFile::begin) - m_offset;
		} else { // If we can reuse the buffer, we have nothing special to do
			newPos = ULONGLONG(lOff);
		}
	} else { // No buffer, we can use the base implementation
		if (nFrom == CStdioFile::begin) {
			lOff += m_offset;
		}
		newPos = m_pStdioFile->Seek(lOff, nFrom) - m_offset;
	}

	m_posInFile = newPos + m_offset + (m_nInBuffer - m_posInBuffer);

	return newPos;
}

void CTextFile::WriteString(LPCSTR lpsz/*CStringA str*/)
{
	if (!m_pStdioFile) {
		return;
	}

	CStringA str(lpsz);

	if (m_encoding == ASCII) {
		m_pStdioFile->WriteString(AToT(str));
	} else if (m_encoding == ANSI) {
		str.Replace("\n", "\r\n");
		m_pStdioFile->Write(str.GetString(), str.GetLength());
	} else if (m_encoding == UTF8) {
		WriteString(AToT(str));
	} else if (m_encoding == LE16) {
		WriteString(AToT(str));
	} else if (m_encoding == BE16) {
		WriteString(AToT(str));
	}
}

void CTextFile::WriteString(LPCWSTR lpsz/*CStringW str*/)
{
	if (!m_pStdioFile) {
		return;
	}

	CStringW str(lpsz);

	if (m_encoding == ASCII) {
		m_pStdioFile->WriteString(str);
	} else if (m_encoding == ANSI) {
		str.Replace(L"\n", L"\r\n");
		CStringA stra(str); // TODO: codepage
		m_pStdioFile->Write(stra.GetString(), stra.GetLength());
	} else if (m_encoding == UTF8) {
		str.Replace(L"\n", L"\r\n");
		for (unsigned int i = 0, l = str.GetLength(); i < l; i++) {
			DWORD c = (WORD)str[i];

			if (c < 0x80) { // 0xxxxxxx
				m_pStdioFile->Write(&c, 1);
			} else if (c < 0x800) { // 110xxxxx 10xxxxxx
				c = 0xc080 | ((c << 2) & 0x1f00) | (c & 0x003f);
				m_pStdioFile->Write((BYTE*)&c + 1, 1);
				m_pStdioFile->Write(&c, 1);
			} else if (c < 0xFFFF) { // 1110xxxx 10xxxxxx 10xxxxxx
				c = 0xe08080 | ((c << 4) & 0x0f0000) | ((c << 2) & 0x3f00) | (c & 0x003f);
				m_pStdioFile->Write((BYTE*)&c + 2, 1);
				m_pStdioFile->Write((BYTE*)&c + 1, 1);
				m_pStdioFile->Write(&c, 1);
			} else {
				c = '?';
				m_pStdioFile->Write(&c, 1);
			}
		}
	} else if (m_encoding == LE16) {
		str.Replace(L"\n", L"\r\n");
		m_pStdioFile->Write(str.GetString(), str.GetLength() * 2);
	} else if (m_encoding == BE16) {
		str.Replace(L"\n", L"\r\n");
		for (unsigned int i = 0, l = str.GetLength(); i < l; i++) {
			str.SetAt(i, ((str[i] >> 8) & 0x00ff) | ((str[i] << 8) & 0xff00));
		}
		m_pStdioFile->Write(str.GetString(), str.GetLength() * 2);
	}
}

bool CTextFile::FillBuffer()
{
	if (!m_pStdioFile) {
		return false;
	}

	if (m_posInBuffer < m_nInBuffer) {
		m_nInBuffer -= m_posInBuffer;
		memcpy(m_buffer.get(), &m_buffer[m_posInBuffer], (size_t)m_nInBuffer * sizeof(char));
	} else {
		m_nInBuffer = 0;
	}
	m_posInBuffer = 0;

	UINT nBytesRead = m_pStdioFile->Read(&m_buffer[m_nInBuffer], UINT(TEXTFILE_BUFFER_SIZE - m_nInBuffer) * sizeof(char));
	if (nBytesRead) {
		m_nInBuffer += nBytesRead;
	}
	m_posInFile = m_pStdioFile->GetPosition();

	return !nBytesRead;
}

ULONGLONG CTextFile::GetPositionFastBuffered() const
{
	return m_pStdioFile ? (m_posInFile - m_offset - (m_nInBuffer - m_posInBuffer)) : 0ULL;
}

bool CTextFile::ReadString(CStringA& str)
{
	if (!m_pStdioFile) {
		return false;
	}

	bool fEOF = true;

	str.Truncate(0);

	if (m_encoding == ASCII) {
		CStringW s;
		fEOF = !m_pStdioFile->ReadString(s);
		str = TToA(s);
		// For consistency with other encodings, we continue reading
		// the file even when a NUL char is encountered.
		char c;
		while (fEOF && (m_pStdioFile->Read(&c, sizeof(c)) == sizeof(c))) {
			str += c;
			fEOF = !m_pStdioFile->ReadString(s);
			str += TToA(s);
		}
	} else if (m_encoding == ANSI) {
		bool bLineEndFound = false;
		fEOF = false;

		do {
			int nCharsRead;

			for (nCharsRead = 0; m_posInBuffer + nCharsRead < m_nInBuffer; nCharsRead++) {
				if (m_buffer[m_posInBuffer + nCharsRead] == '\n') {
					break;
				} else if (m_buffer[m_posInBuffer + nCharsRead] == '\r') {
					break;
				}
			}

			str.Append(&m_buffer[m_posInBuffer], nCharsRead);

			m_posInBuffer += nCharsRead;
			while (m_posInBuffer < m_nInBuffer && m_buffer[m_posInBuffer] == '\r') {
				m_posInBuffer++;
			}
			if (m_posInBuffer < m_nInBuffer && m_buffer[m_posInBuffer] == '\n') {
				bLineEndFound = true; // Stop at end of line
				m_posInBuffer++;
			}

			if (!bLineEndFound) {
				bLineEndFound = FillBuffer();
				if (!nCharsRead) {
					fEOF = bLineEndFound;
				}
			}
		} while (!bLineEndFound);
	} else if (m_encoding == UTF8) {
		ULONGLONG lineStartPos = GetPositionFastBuffered();
		bool bValid = true;
		bool bLineEndFound = false;
		fEOF = false;

		do {
			int nCharsRead;
			char* abuffer = (char*)m_wbuffer.get();

			for (nCharsRead = 0; m_posInBuffer < m_nInBuffer; m_posInBuffer++, nCharsRead++) {
				if (Utf8::isSingleByte(m_buffer[m_posInBuffer])) { // 0xxxxxxx
					abuffer[nCharsRead] = m_buffer[m_posInBuffer] & 0x7f;
				} else if (Utf8::isFirstOfMultibyte(m_buffer[m_posInBuffer])) {
					int nContinuationBytes = Utf8::continuationBytes(m_buffer[m_posInBuffer]);
					bValid = (nContinuationBytes <= 2);

					// We don't support characters wider than 16 bits
					if (bValid) {
						if (m_posInBuffer + nContinuationBytes >= m_nInBuffer) {
							// If we are at the end of the file, the buffer won't be full
							// and we won't be able to read any more continuation bytes.
							bValid = (m_nInBuffer == TEXTFILE_BUFFER_SIZE);
							break;
						} else {
							for (int j = 1; j <= nContinuationBytes; j++) {
								if (!Utf8::isContinuation(m_buffer[m_posInBuffer + j])) {
									bValid = false;
								}
							}

							switch (nContinuationBytes) {
								case 0: // 0xxxxxxx
									abuffer[nCharsRead] = m_buffer[m_posInBuffer] & 0x7f;
									break;
								case 1: // 110xxxxx 10xxxxxx
								case 2: // 1110xxxx 10xxxxxx 10xxxxxx
									// Unsupported for non unicode strings
									abuffer[nCharsRead] = '?';
									break;
							}
							m_posInBuffer += nContinuationBytes;
						}
					}
				} else {
					bValid = false;
				}

				if (!bValid) {
					abuffer[nCharsRead] = '?';
					m_posInBuffer++;
					nCharsRead++;
					break;
				} else if (abuffer[nCharsRead] == '\n') {
					bLineEndFound = true; // Stop at end of line
					m_posInBuffer++;
					break;
				} else if (abuffer[nCharsRead] == '\r') {
					nCharsRead--; // Skip \r
				}
			}

			if (bValid || m_offset) {
				str.Append(abuffer, nCharsRead);

				if (!bLineEndFound) {
					bLineEndFound = FillBuffer();
					if (!nCharsRead) {
						fEOF = bLineEndFound;
					}
				}
			} else {
				// Switch to text and read again
				m_encoding = m_defaultencoding;
				// Stop using the buffer
				m_posInBuffer = m_nInBuffer = 0;

				fEOF = !ReopenAsText();

				if (!fEOF) {
					// Seek back to the beginning of the line where we stopped
					Seek(lineStartPos, CStdioFile::begin);

					fEOF = !ReadString(str);
				}
			}
		} while (bValid && !bLineEndFound);
	} else if (m_encoding == LE16) {
		bool bLineEndFound = false;
		fEOF = false;

		do {
			int nCharsRead;
			WCHAR* wbuffer = (WCHAR*)&m_buffer[m_posInBuffer];
			char* abuffer = (char*)m_wbuffer.get();

			for (nCharsRead = 0; m_posInBuffer + 1 < m_nInBuffer; nCharsRead++, m_posInBuffer += sizeof(WCHAR)) {
				if (wbuffer[nCharsRead] == L'\n') {
					break; // Stop at end of line
				} else if (wbuffer[nCharsRead] == L'\r') {
					break; // Skip \r
				} else if (!(wbuffer[nCharsRead] & 0xff00)) {
					abuffer[nCharsRead] = char(wbuffer[nCharsRead] & 0xff);
				} else {
					abuffer[nCharsRead] = '?';
				}
			}

			str.Append(abuffer, nCharsRead);

			while (m_posInBuffer + 1 < m_nInBuffer && wbuffer[nCharsRead] == L'\r') {
				nCharsRead++;
				m_posInBuffer += sizeof(WCHAR);
			}
			if (m_posInBuffer + 1 < m_nInBuffer && wbuffer[nCharsRead] == L'\n') {
				bLineEndFound = true; // Stop at end of line
				nCharsRead++;
				m_posInBuffer += sizeof(WCHAR);
			}

			if (!bLineEndFound) {
				bLineEndFound = FillBuffer();
				if (!nCharsRead) {
					fEOF = bLineEndFound;
				}
			}
		} while (!bLineEndFound);
	} else if (m_encoding == BE16) {
		bool bLineEndFound = false;
		fEOF = false;

		do {
			int nCharsRead;
			char* abuffer = (char*)m_wbuffer.get();

			for (nCharsRead = 0; m_posInBuffer + 1 < m_nInBuffer; nCharsRead++, m_posInBuffer += sizeof(WCHAR)) {
				if (!m_buffer[m_posInBuffer]) {
					abuffer[nCharsRead] = m_buffer[m_posInBuffer + 1];
				} else {
					abuffer[nCharsRead] = '?';
				}

				if (abuffer[nCharsRead] == '\n') {
					bLineEndFound = true; // Stop at end of line
					m_posInBuffer += sizeof(WCHAR);
					break;
				} else if (abuffer[nCharsRead] == L'\r') {
					nCharsRead--; // Skip \r
				}
			}

			str.Append(abuffer, nCharsRead);

			if (!bLineEndFound) {
				bLineEndFound = FillBuffer();
				if (!nCharsRead) {
					fEOF = bLineEndFound;
				}
			}
		} while (!bLineEndFound);
	}

	return !fEOF;
}

bool CTextFile::ReadString(CStringW& str)
{
	if (!m_pStdioFile) {
		return false;
	}

	bool fEOF = true;

	str.Truncate(0);

	if (m_encoding == ASCII) {
		CStringW s;
		fEOF = !m_pStdioFile->ReadString(s);
		str = s;
		// For consistency with other encodings, we continue reading
		// the file even when a NUL char is encountered.
		char c;
		while (fEOF && (m_pStdioFile->Read(&c, sizeof(c)) == sizeof(c))) {
			str += c;
			fEOF = !m_pStdioFile->ReadString(s);
			str += s;
		}
	} else if (m_encoding == ANSI) {
		bool bLineEndFound = false;
		fEOF = false;

		do {
			int nCharsRead;

			for (nCharsRead = 0; m_posInBuffer + nCharsRead < m_nInBuffer; nCharsRead++) {
				if (m_buffer[m_posInBuffer + nCharsRead] == '\n') {
					break;
				} else if (m_buffer[m_posInBuffer + nCharsRead] == '\r') {
					break;
				}
			}

			// TODO: codepage
			str.Append(CStringW(&m_buffer[m_posInBuffer], nCharsRead));

			m_posInBuffer += nCharsRead;
			while (m_posInBuffer < m_nInBuffer && m_buffer[m_posInBuffer] == '\r') {
				m_posInBuffer++;
			}
			if (m_posInBuffer < m_nInBuffer && m_buffer[m_posInBuffer] == '\n') {
				bLineEndFound = true; // Stop at end of line
				m_posInBuffer++;
			}

			if (!bLineEndFound) {
				bLineEndFound = FillBuffer();
				if (!nCharsRead) {
					fEOF = bLineEndFound;
				}
			}
		} while (!bLineEndFound);
	} else if (m_encoding == UTF8) {
		ULONGLONG lineStartPos = GetPositionFastBuffered();
		bool bValid = true;
		bool bLineEndFound = false;
		fEOF = false;

		do {
			int nCharsRead;

			for (nCharsRead = 0; m_posInBuffer < m_nInBuffer; m_posInBuffer++, nCharsRead++) {
				if (Utf8::isSingleByte(m_buffer[m_posInBuffer])) { // 0xxxxxxx
					m_wbuffer[nCharsRead] = m_buffer[m_posInBuffer] & 0x7f;
				} else if (Utf8::isFirstOfMultibyte(m_buffer[m_posInBuffer])) {
					int nContinuationBytes = Utf8::continuationBytes(m_buffer[m_posInBuffer]);
					bValid = true;

					if (m_posInBuffer + nContinuationBytes >= m_nInBuffer) {
						// If we are at the end of the file, the buffer won't be full
						// and we won't be able to read any more continuation bytes.
						bValid = (m_nInBuffer == TEXTFILE_BUFFER_SIZE);
						break;
					} else {
						for (int j = 1; j <= nContinuationBytes; j++) {
							if (!Utf8::isContinuation(m_buffer[m_posInBuffer + j])) {
								bValid = false;
							}
						}

						switch (nContinuationBytes) {
							case 0: // 0xxxxxxx
								m_wbuffer[nCharsRead] = m_buffer[m_posInBuffer] & 0x7f;
								break;
							case 1: // 110xxxxx 10xxxxxx
								m_wbuffer[nCharsRead] = (m_buffer[m_posInBuffer] & 0x1f) << 6 | (m_buffer[m_posInBuffer + 1] & 0x3f);
								break;
							case 2: // 1110xxxx 10xxxxxx 10xxxxxx
								m_wbuffer[nCharsRead] = (m_buffer[m_posInBuffer] & 0x0f) << 12 | (m_buffer[m_posInBuffer + 1] & 0x3f) << 6 | (m_buffer[m_posInBuffer + 2] & 0x3f);
								break;
							case 3: // 11110xxx 10xxxxxx 10xxxxxx 10xxxxxx
								{
									const auto* Z = &m_buffer[m_posInBuffer];
									const auto u32 = ((uint32_t)(*Z & 0x0F) << 18) | ((uint32_t)(*(Z + 1) & 0x3F) << 12) | ((uint32_t)(*(Z + 2) & 0x3F) << 6) | ((uint32_t) * (Z + 3) & 0x3F);
									if (u32 <= UINT16_MAX) {
										m_wbuffer[nCharsRead] = (wchar_t)u32;
									} else {
										m_wbuffer[nCharsRead++] = (wchar_t)((((u32 - 0x010000) & 0x000FFC00) >> 10) | 0xD800);
										m_wbuffer[nCharsRead]   = (wchar_t)((u32 & 0x000003FF) | 0xDC00);
									}
								}
								break;
						}
						m_posInBuffer += nContinuationBytes;
					}
				} else {
					bValid = false;
				}

				if (!bValid) {
					m_wbuffer[nCharsRead] = L'?';
					m_posInBuffer++;
					nCharsRead++;
					break;
				} else if (m_wbuffer[nCharsRead] == L'\n') {
					bLineEndFound = true; // Stop at end of line
					m_posInBuffer++;
					break;
				} else if (m_wbuffer[nCharsRead] == L'\r') {
					nCharsRead--; // Skip \r
				}
			}

			if (bValid || m_offset) {
				str.Append(m_wbuffer.get(), nCharsRead);

				if (!bLineEndFound) {
					bLineEndFound = FillBuffer();
					if (!nCharsRead) {
						fEOF = bLineEndFound;
					}
				}
			} else {
				// Switch to text and read again
				m_encoding = m_defaultencoding;
				// Stop using the buffer
				m_posInBuffer = m_nInBuffer = 0;

				fEOF = !ReopenAsText();

				if (!fEOF) {
					// Seek back to the beginning of the line where we stopped
					Seek(lineStartPos, CStdioFile::begin);

					fEOF = !ReadString(str);
				}
			}
		} while (bValid && !bLineEndFound);
	} else if (m_encoding == LE16) {
		bool bLineEndFound = false;
		fEOF = false;

		do {
			int nCharsRead;
			WCHAR* wbuffer = (WCHAR*)&m_buffer[m_posInBuffer];

			for (nCharsRead = 0; m_posInBuffer + 1 < m_nInBuffer; nCharsRead++, m_posInBuffer += sizeof(WCHAR)) {
				if (wbuffer[nCharsRead] == L'\n') {
					break; // Stop at end of line
				} else if (wbuffer[nCharsRead] == L'\r') {
					break; // Skip \r
				}
			}

			str.Append(wbuffer, nCharsRead);

			while (m_posInBuffer + 1 < m_nInBuffer && wbuffer[nCharsRead] == L'\r') {
				nCharsRead++;
				m_posInBuffer += sizeof(WCHAR);
			}
			if (m_posInBuffer + 1 < m_nInBuffer && wbuffer[nCharsRead] == L'\n') {
				bLineEndFound = true; // Stop at end of line
				nCharsRead++;
				m_posInBuffer += sizeof(WCHAR);
			}

			if (!bLineEndFound) {
				bLineEndFound = FillBuffer();
				if (!nCharsRead) {
					fEOF = bLineEndFound;
				}
			}
		} while (!bLineEndFound);
	} else if (m_encoding == BE16) {
		bool bLineEndFound = false;
		fEOF = false;

		do {
			int nCharsRead;

			for (nCharsRead = 0; m_posInBuffer + 1 < m_nInBuffer; nCharsRead++, m_posInBuffer += sizeof(WCHAR)) {
				m_wbuffer[nCharsRead] = ((WCHAR(m_buffer[m_posInBuffer]) << 8) & 0xff00) | (WCHAR(m_buffer[m_posInBuffer + 1]) & 0x00ff);
				if (m_wbuffer[nCharsRead] == L'\n') {
					bLineEndFound = true; // Stop at end of line
					m_posInBuffer += sizeof(WCHAR);
					break;
				} else if (m_wbuffer[nCharsRead] == L'\r') {
					nCharsRead--; // Skip \r
				}
			}

			str.Append(m_wbuffer.get(), nCharsRead);

			if (!bLineEndFound) {
				bLineEndFound = FillBuffer();
				if (!nCharsRead) {
					fEOF = bLineEndFound;
				}
			}
		} while (!bLineEndFound);
	}

	return !fEOF;
}

//
// CWebTextFile
//

CWebTextFile::CWebTextFile(CTextFile::enc encoding/* = ASCII*/, CTextFile::enc defaultencoding/* = ASCII*/, LONGLONG llMaxSize)
	: CTextFile(encoding, defaultencoding)
	, m_llMaxSize(llMaxSize)
{
}

CWebTextFile::~CWebTextFile()
{
	Close();
}

bool CWebTextFile::Open(LPCWSTR lpszFileName)
{
	CStringW fn(lpszFileName);

	if (fn.Find(L"http://") != 0 && fn.Find(L"https://") != 0) {
		return __super::Open(lpszFileName);
	}

	CHTTPAsync HTTPAsync;
	if (SUCCEEDED(HTTPAsync.Connect(lpszFileName, http::connectTimeout))) {
		if (GetTemporaryFilePath(L".tmp", fn)) {
			CFile temp;
			if (!temp.Open(fn, CStdioFile::modeCreate | CStdioFile::modeWrite | CStdioFile::typeBinary | CStdioFile::shareDenyWrite)) {
				HTTPAsync.Close();
				return false;
			}

			if (HTTPAsync.IsCompressed()) {
				if (HTTPAsync.GetLenght() <= 10 * MEGABYTE) {
					std::vector<BYTE> body;
					if (HTTPAsync.GetUncompressed(body)) {
						temp.Write(body.data(), static_cast<UINT>(body.size()));
						m_tempfn = fn;
					}
				}
			} else {
				BYTE buffer[1024] = {};
				DWORD dwSizeRead = 0;
				DWORD totalSize = 0;
				do {
					if (HTTPAsync.Read(buffer, 1024, dwSizeRead, http::readTimeout) != S_OK) {
						break;
					}
					temp.Write(buffer, dwSizeRead);
					totalSize += dwSizeRead;
				} while (dwSizeRead && totalSize < m_llMaxSize);
				temp.Close();

				if (totalSize) {
					m_tempfn = fn;
				}
			}
		}

		m_url_redirect_str = HTTPAsync.GetRedirectURL();
		HTTPAsync.Close();
	}

	return __super::Open(m_tempfn);
}

void CWebTextFile::Close()
{
	__super::Close();

	if (!m_tempfn.IsEmpty()) {
		_wremove(m_tempfn);
		m_tempfn.Empty();
	}
}

const CString& CWebTextFile::GetRedirectURL() const
{
	return m_url_redirect_str;
}

///////////////////////////////////////////////////////////////

CStringW AToT(CStringA str)
{
	CStringW ret;
	for (int i = 0, j = str.GetLength(); i < j; i++) {
		ret += (WCHAR)(BYTE)str[i];
	}
	return ret;
}

CStringA TToA(CStringW str)
{
	CStringA ret;
	for (int i = 0, j = str.GetLength(); i < j; i++) {
		ret += (CHAR)(BYTE)str[i];
	}
	return ret;
}
