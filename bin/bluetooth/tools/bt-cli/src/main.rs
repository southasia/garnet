// Copyright 2018 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#![feature(async_await, await_macro, futures_api)]
#![deny(warnings)]

extern crate fuchsia_app as app;
extern crate fuchsia_async as fasync;
extern crate fuchsia_bluetooth as bluetooth;

use crate::app::client::connect_to_service;
use crate::bluetooth::types::Status;
use crate::commands::{Cmd, CmdCompleter};
use failure::ResultExt;
use failure::Error;
use fidl_fuchsia_bluetooth_control::{ControlEvent, ControlMarker, ControlProxy, ControlEventStream};
use crate::fasync::futures::{TryFutureExt, StreamExt};
use parking_lot::RwLock;
use rustyline::{CompletionType, Config, EditMode, Editor};
use rustyline::error::ReadlineError;
use std::fmt::Write;
use std::sync::Arc;
use crate::types::AdapterInfo;

mod commands;
mod types;

static PROMPT: &'static str = "\x1b[34mbt>\x1b[0m ";

async fn get_active_adapter(control_svc: Arc<RwLock<ControlProxy>>) -> Result<String, Error> {
    if let Some(adapter) = await!(control_svc.read().get_active_adapter_info())? {
        return Ok(AdapterInfo::from(*adapter).to_string())
    }
    Ok(String::from("No Active Adapter"))
}

async fn get_adapters(control_svc: Arc<RwLock<ControlProxy>>) -> Result<String, Error> {
    if let Some(adapters) = await!(control_svc.read().get_adapters())? {
        let mut string = String::new();
        for adapter in adapters {
            let _ = writeln!(string, "{}", AdapterInfo::from(adapter));
        }
        return Ok(string)
    }
    Ok(String::from("No adapters detected"))
}

async fn start_discovery(control_svc: Arc<RwLock<ControlProxy>>) -> Result<String, Error> {
    let response = await!(control_svc.read().request_discovery(true))?;
    Ok(Status::from(response).to_string())
}

async fn stop_discovery(control_svc: Arc<RwLock<ControlProxy>>) -> Result<String, Error> {
    let response = await!(control_svc.read().request_discovery(false))?;
    Ok(Status::from(response).to_string())
}

async fn set_discoverable(discoverable: bool, control_svc: Arc<RwLock<ControlProxy>>) -> Result<String, Error> {
    let response = await!(control_svc.read().set_discoverable(discoverable))?;
    Ok(Status::from(response).to_string())
}

async fn run_listeners(mut stream: ControlEventStream) -> Result<(), Error> {
    while let Some(evt) = await!(stream.next()) {
        match evt? {
            ControlEvent::OnAdapterUpdated { adapter } => {
                eprintln!("Adapter {} updated", adapter.identifier);
            }
            ControlEvent::OnDeviceUpdated { device } => {
                eprintln!("Device: {:#?}", device);
            }
            ControlEvent::OnDeviceRemoved { identifier } => {
                eprintln!("Device {} removed", identifier);
            }
            _ => eprintln!("Unknown Event"),
        }
    }
    Ok(())
}

fn main() -> Result<(), Error> {
    let config = Config::builder()
        .history_ignore_space(true)
        .completion_type(CompletionType::List)
        .edit_mode(EditMode::Emacs)
        .build();
    let c = CmdCompleter::new();
    let mut rl = Editor::with_config(config);
    rl.set_completer(Some(c));

    let mut exec = fasync::Executor::new().context("error creating event loop")?;
    let bt_svc = Arc::new(RwLock::new(connect_to_service::<ControlMarker>()
        .context("failed to connect to bluetooth control interface")?));

    let bt_svc_thread = bt_svc.clone();
    let evt_stream = bt_svc_thread.read().take_event_stream();

    fasync::spawn(run_listeners(evt_stream).unwrap_or_else(|e| eprintln!("Failed to subscribe to bluetooth events {:?}", e)));

    // start the repl
    loop {
        let readline = rl.readline(PROMPT);
        match readline {
            Ok(line) => {
                let cmd = line.parse::<Cmd>();
                let fut = async {
                    match cmd {
                        Ok(Cmd::StartDiscovery) => {
                            println!("Starting Discovery!");
                            await!(start_discovery(bt_svc.clone()))
                        }
                        Ok(Cmd::StopDiscovery) => {
                            println!("Stopping Discovery!");
                            await!(stop_discovery(bt_svc.clone()))
                        }
                        Ok(Cmd::Discoverable) => {
                            println!("Becoming discoverable..");
                            await!(set_discoverable(true, bt_svc.clone()))
                        }
                        Ok(Cmd::NotDiscoverable) => {
                            println!("Revoking discoverability..");
                            await!(set_discoverable(false, bt_svc.clone()))
                        }
                        Ok(Cmd::GetAdapters) => await!(get_adapters(bt_svc.clone())),
                        Ok(Cmd::ActiveAdapter) =>  await!(get_active_adapter(bt_svc.clone())),
                        Ok(Cmd::Help) => Ok(Cmd::help_msg()),
                        Ok(Cmd::Nothing) => Ok(String::from("")),
                        Err(e) => Ok(format!("Error: {:?}", e)),
                    }
                };
                let res = exec.run_singlethreaded(fut)?;
                if res != "" {
                    println!("{}", res);
                }
            }
            Err(ReadlineError::Interrupted) => break,
            Err(_) => break, // empty line
        }
    }
    Ok(())
}
