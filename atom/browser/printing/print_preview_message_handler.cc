// Copyright (c) 2018 GitHub, Inc.
// Use of this source code is governed by the MIT license that can be
// found in the LICENSE file.

#include "atom/browser/printing/print_preview_message_handler.h"

#include <memory>
#include <utility>

#include "base/bind.h"
#include "base/memory/ref_counted.h"
#include "base/memory/shared_memory.h"
#include "base/memory/shared_memory_handle.h"
#include "chrome/browser/browser_process.h"
#include "chrome/browser/printing/print_job_manager.h"
#include "chrome/browser/printing/printer_query.h"
#include "components/printing/common/print_messages.h"
#include "content/public/browser/browser_thread.h"
#include "content/public/browser/render_frame_host.h"
#include "content/public/browser/web_contents.h"

#include "atom/common/node_includes.h"

using content::BrowserThread;

DEFINE_WEB_CONTENTS_USER_DATA_KEY(atom::PrintPreviewMessageHandler);

namespace atom {

namespace {

void StopWorker(int document_cookie) {
  if (document_cookie <= 0)
    return;
  scoped_refptr<printing::PrintQueriesQueue> queue =
      g_browser_process->print_job_manager()->queue();
  scoped_refptr<printing::PrinterQuery> printer_query =
      queue->PopPrinterQuery(document_cookie);
  if (printer_query.get()) {
    BrowserThread::PostTask(
        BrowserThread::IO, FROM_HERE,
        base::BindOnce(&printing::PrinterQuery::StopWorker, printer_query));
  }
}

char* CopyPDFDataOnIOThread(
    const PrintHostMsg_DidPreviewDocument_Params& params) {
  DCHECK_CURRENTLY_ON(BrowserThread::IO);
  const PrintHostMsg_DidPrintContent_Params& content = params.content;
  std::unique_ptr<base::SharedMemory> shared_buf(
      new base::SharedMemory(content.metafile_data_handle, true));
  if (!shared_buf->Map(content.data_size))
    return nullptr;
  char* pdf_data = new char[content.data_size];
  memcpy(pdf_data, shared_buf->memory(), content.data_size);
  return pdf_data;
}

void FreeNodeBufferData(char* data, void* hint) {
  delete[] data;
}

}  // namespace

PrintPreviewMessageHandler::PrintPreviewMessageHandler(
    content::WebContents* web_contents)
    : content::WebContentsObserver(web_contents), weak_ptr_factory_(this) {
  DCHECK(web_contents);
}

PrintPreviewMessageHandler::~PrintPreviewMessageHandler() {}

bool PrintPreviewMessageHandler::OnMessageReceived(
    const IPC::Message& message,
    content::RenderFrameHost* render_frame_host) {
  bool handled = true;
  IPC_BEGIN_MESSAGE_MAP_WITH_PARAM(PrintPreviewMessageHandler, message,
                                   render_frame_host)
    IPC_MESSAGE_HANDLER(PrintHostMsg_MetafileReadyForPrinting,
                        OnMetafileReadyForPrinting)
    IPC_MESSAGE_UNHANDLED(handled = false)
  IPC_END_MESSAGE_MAP()
  if (handled)
    return true;

  handled = true;
  IPC_BEGIN_MESSAGE_MAP(PrintPreviewMessageHandler, message)
    IPC_MESSAGE_HANDLER(PrintHostMsg_PrintPreviewFailed, OnPrintPreviewFailed)
    IPC_MESSAGE_UNHANDLED(handled = false)
  IPC_END_MESSAGE_MAP()
  return handled;
}

void PrintPreviewMessageHandler::OnMetafileReadyForPrinting(
    content::RenderFrameHost* render_frame_host,
    const PrintHostMsg_DidPreviewDocument_Params& params,
    const PrintHostMsg_PreviewIds& ids) {
  // Always try to stop the worker.
  StopWorker(params.document_cookie);

  if (params.expected_pages_count <= 0) {
    NOTREACHED();
    return;
  }

  BrowserThread::PostTaskAndReplyWithResult(
      BrowserThread::IO, FROM_HERE, base::Bind(&CopyPDFDataOnIOThread, params),
      base::Bind(&PrintPreviewMessageHandler::RunPrintToPDFCallback,
                 base::Unretained(this), ids.request_id,
                 params.content.data_size));
}

void PrintPreviewMessageHandler::OnPrintPreviewFailed(
    int document_cookie,
    const PrintHostMsg_PreviewIds& ids) {
  StopWorker(document_cookie);

  RunPrintToPDFCallback(ids.request_id, 0, nullptr);
}

void PrintPreviewMessageHandler::PrintToPDF(
    const base::DictionaryValue& options,
    const PrintToPDFCallback& callback) {
  int request_id;
  options.GetInteger(printing::kPreviewRequestID, &request_id);
  print_to_pdf_callback_map_[request_id] = callback;

  content::RenderFrameHost* rfh = web_contents()->GetMainFrame();
  rfh->Send(new PrintMsg_PrintPreview(rfh->GetRoutingID(), options));
}

void PrintPreviewMessageHandler::RunPrintToPDFCallback(int request_id,
                                                       uint32_t data_size,
                                                       char* data) {
  DCHECK_CURRENTLY_ON(BrowserThread::UI);

  v8::Isolate* isolate = v8::Isolate::GetCurrent();
  v8::Locker locker(isolate);
  v8::HandleScope handle_scope(isolate);
  if (data) {
    v8::Local<v8::Value> buffer =
        node::Buffer::New(isolate, data, static_cast<size_t>(data_size),
                          &FreeNodeBufferData, nullptr)
            .ToLocalChecked();
    print_to_pdf_callback_map_[request_id].Run(v8::Null(isolate), buffer);
  } else {
    v8::Local<v8::String> error_message =
        v8::String::NewFromUtf8(isolate, "Failed to generate PDF");
    print_to_pdf_callback_map_[request_id].Run(
        v8::Exception::Error(error_message), v8::Null(isolate));
  }
  print_to_pdf_callback_map_.erase(request_id);
}

}  // namespace atom
