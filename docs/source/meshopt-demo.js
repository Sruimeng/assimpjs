const state = {
  ajs: null,
  files: []
};

const dropzone = document.getElementById("dropzone");
const fileInput = document.getElementById("fileInput");
const fileListEl = document.getElementById("fileList");
const statusEl = document.getElementById("status");
const logEl = document.getElementById("log");
const outputsEl = document.getElementById("outputs");
const convertBtn = document.getElementById("convertBtn");
const clearBtn = document.getElementById("clearBtn");
const projectNameInput = document.getElementById("projectName");
const metallicInput = document.getElementById("metallic");
const roughnessInput = document.getElementById("roughness");
const transformMatrixInput = document.getElementById("transformMatrix");
const childrenRenameInput = document.getElementById("childrenRename");
const childrenDeletedInput = document.getElementById("childrenDeleted");
const childrenTransformMatrixInput = document.getElementById("childrenTransformMatrix");

const mainExtensions = new Set(["glb", "gltf", "fbx"]);

function setStatus(message, tone) {
  statusEl.textContent = message;
  statusEl.dataset.tone = tone || "idle";
}

function logLine(message) {
  const time = new Date().toLocaleTimeString();
  logEl.textContent = `[${time}] ${message}\n` + logEl.textContent;
}

function formatBytes(bytes) {
  if (bytes === 0) return "0 B";
  const units = ["B", "KB", "MB", "GB"]; 
  const idx = Math.min(units.length - 1, Math.floor(Math.log(bytes) / Math.log(1024)));
  return `${(bytes / Math.pow(1024, idx)).toFixed(idx === 0 ? 0 : 2)} ${units[idx]}`;
}

function getExtension(name) {
  const parts = name.toLowerCase().split(".");
  return parts.length > 1 ? parts.pop() : "";
}

function hasSupportedMain(files) {
  return files.some((file) => mainExtensions.has(getExtension(file.name)));
}

function updateFileList(files) {
  fileListEl.innerHTML = "";
  if (!files.length) {
    fileListEl.innerHTML = "<div class=\"file-item\">No files loaded yet.</div>";
    return;
  }
  files.forEach((file) => {
    const item = document.createElement("div");
    item.className = "file-item";
    const ext = getExtension(file.name) || "unknown";
    item.innerHTML = `<div>${file.name}</div><span>${ext} · ${formatBytes(file.size)}</span>`;
    fileListEl.appendChild(item);
  });
}

function setFiles(fileList) {
  state.files = Array.from(fileList || []);
  updateFileList(state.files);
  outputsEl.innerHTML = "";
  if (state.files.length === 0) {
    setStatus("Waiting for files", "idle");
    return;
  }
  if (!hasSupportedMain(state.files)) {
    setStatus("Drop a GLB/GLTF/FBX as main file", "error");
  } else {
    setStatus("Files ready", "idle");
  }
}

function getFileBuffer(file) {
  return new Promise((resolve, reject) => {
    const reader = new FileReader();
    reader.onloadend = (event) => {
      if (event.target.readyState === FileReader.DONE) {
        resolve(event.target.result);
      }
    };
    reader.onerror = () => reject(new Error("File read failed"));
    reader.readAsArrayBuffer(file);
  });
}

function downloadFile(content, filename) {
  const blob = new Blob([content], { type: "application/octet-stream" });
  const url = URL.createObjectURL(blob);
  const link = document.createElement("a");
  link.href = url;
  link.download = filename;
  document.body.appendChild(link);
  link.click();
  document.body.removeChild(link);
  URL.revokeObjectURL(url);
}

function parseJsonInput(value, fallback) {
  if (!value || !value.trim()) return fallback;
  try {
    return JSON.parse(value);
  } catch (error) {
    throw new Error(`Invalid JSON: ${error.message}`);
  }
}

function buildMetadata() {
  const meta = {};
  let hasAny = false;

  const transformMatrix = parseJsonInput(transformMatrixInput.value, null);
  if (transformMatrix) {
    meta.transform_matrix = transformMatrix;
    hasAny = true;
  }

  const childrenRename = parseJsonInput(childrenRenameInput.value, null);
  if (childrenRename) {
    meta.children_rename = childrenRename;
    hasAny = true;
  }

  const childrenDeleted = parseJsonInput(childrenDeletedInput.value, null);
  if (childrenDeleted) {
    meta.children_deleted = childrenDeleted;
    hasAny = true;
  }

  const childrenTransformMatrix = parseJsonInput(childrenTransformMatrixInput.value, null);
  if (childrenTransformMatrix) {
    meta.children_transform_matrix = childrenTransformMatrix;
    hasAny = true;
  }

  const metallicValue = metallicInput.value.trim();
  const roughnessValue = roughnessInput.value.trim();
  if (metallicValue !== "" || roughnessValue !== "") {
    meta.material_factor = {
      metallic: metallicValue === "" ? 0 : parseFloat(metallicValue),
      roughness: roughnessValue === "" ? 0.5 : parseFloat(roughnessValue)
    };
    hasAny = true;
  }

  return hasAny ? meta : undefined;
}

async function convertFiles() {
  if (!state.ajs) {
    setStatus("WASM not ready", "error");
    return;
  }
  if (!state.files.length) {
    setStatus("Drop files to convert", "error");
    return;
  }
  if (!hasSupportedMain(state.files)) {
    setStatus("Missing GLB/GLTF/FBX main file", "error");
    return;
  }

  const format = document.querySelector("input[name='format']:checked").value;
  setStatus("Converting...", "busy");
  logLine(`Converting to ${format}...`);

  try {
    const buffers = await Promise.all(state.files.map(getFileBuffer));
    const fileList = new state.ajs.FileList();
    buffers.forEach((buffer, idx) => {
      fileList.AddFile(state.files[idx].name, new Uint8Array(buffer));
    });

    let metadata = undefined;
    try {
      metadata = buildMetadata();
    } catch (err) {
      setStatus("Metadata JSON error", "error");
      logLine(err.message || String(err));
      return;
    }
    const projectName = (projectNameInput.value || "").trim();

    const start = performance.now();
    const result = state.ajs.ConvertFileList(fileList, format, metadata, projectName);
    const elapsed = Math.round(performance.now() - start);

    if (!result.IsSuccess()) {
      const error = result.GetErrorCode();
      setStatus(`Failed (${error})`, "error");
      logLine(`Error: ${error}`);
      return;
    }

    if (result.FileCount() === 0) {
      setStatus("No output files generated", "error");
      logLine("Error: exporter returned no files.");
      return;
    }

    outputsEl.innerHTML = "";
    for (let i = 0; i < result.FileCount(); i++) {
      const file = result.GetFile(i);
      const filename = file.GetPath();
      const content = file.GetContent();
      const item = document.createElement("div");
      item.className = "output-item";
      item.innerHTML = `
        <div>
          <strong>${filename}</strong>
          <span>${formatBytes(content.length)}</span>
        </div>
      `;
      const button = document.createElement("button");
      button.textContent = "Download";
      button.addEventListener("click", () => downloadFile(content, filename));
      item.appendChild(button);
      outputsEl.appendChild(item);
    }

    setStatus(`Done in ${elapsed} ms`, "success");
    logLine(`Success. Output files: ${result.FileCount()}`);
  } catch (error) {
    setStatus("Conversion error", "error");
    logLine(`Exception: ${error.message || error}`);
  }
}

function clearAll() {
  state.files = [];
  fileInput.value = "";
  updateFileList([]);
  outputsEl.innerHTML = "";
  setStatus("Waiting for files", "idle");
}

convertBtn.addEventListener("click", () => {
  convertFiles();
});

clearBtn.addEventListener("click", () => {
  clearAll();
});

fileInput.addEventListener("change", (event) => {
  setFiles(event.target.files);
});

dropzone.addEventListener("dragover", (event) => {
  event.preventDefault();
  dropzone.classList.add("is-dragover");
});

dropzone.addEventListener("dragleave", () => {
  dropzone.classList.remove("is-dragover");
});

dropzone.addEventListener("drop", (event) => {
  event.preventDefault();
  dropzone.classList.remove("is-dragover");
  setFiles(event.dataTransfer.files);
});

fileInput.addEventListener("dragover", (event) => {
  event.preventDefault();
  dropzone.classList.add("is-dragover");
});

fileInput.addEventListener("dragleave", () => {
  dropzone.classList.remove("is-dragover");
});

fileInput.addEventListener("drop", (event) => {
  event.preventDefault();
  dropzone.classList.remove("is-dragover");
  setFiles(event.dataTransfer.files);
});

window.addEventListener("load", () => {
  updateFileList([]);
  setStatus("Loading wasm...", "busy");
  if (typeof assimpjs !== "function") {
    setStatus("assimpjs-meshopt.js not loaded", "error");
    return;
  }
  assimpjs().then((ajs) => {
    state.ajs = ajs;
    dropzone.classList.add("is-ready");
    setStatus("Ready", "success");
    logLine("Meshopt wasm loaded.");
  }).catch((err) => {
    setStatus("Failed to load wasm", "error");
    logLine(`Load error: ${err.message || err}`);
  });
});
