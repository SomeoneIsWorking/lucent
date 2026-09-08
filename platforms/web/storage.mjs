/* Browser-owned private staging. Interpretation and accepted-install publication
 * stay with the native consumer, which sees these files through WasmFS OPFS. */
export async function persistentStorage() {
  if (!globalThis.isSecureContext || !navigator.storage?.getDirectory) {
    throw new Error("This browser cannot provide private persistent storage.");
  }
  const persistent = await navigator.storage.persist();
  return {root: await navigator.storage.getDirectory(), persistent};
}

function leaf(name) {
  if (typeof name !== "string" || !name || name === "." || name === ".." || /[\\/\0]/.test(name)) {
    throw new Error("Staging requires one safe file or directory name.");
  }
  return name;
}

export class FileStager {
  #active = false;

  async stage(file, {directory, name, maxBytes, progress = () => {}}) {
    leaf(directory);
    leaf(name);
    if (!(file instanceof Blob) || !Number.isSafeInteger(maxBytes) || maxBytes <= 0 ||
        file.size <= 0 || file.size > maxBytes) {
      throw new Error("The selected file exceeds the import byte limit or is empty.");
    }
    if (this.#active) throw new Error("An import is already in progress.");
    if (!navigator.locks) throw new Error("This browser cannot protect concurrent imports.");
    this.#active = true;
    try {
      return await navigator.locks.request(`lucent-import:${directory}`, {ifAvailable: true}, async lock => {
        if (!lock) throw new Error("Another tab is importing into this storage directory.");
        return this.#copy(file, {directory, name, maxBytes, progress});
      });
    } finally {
      this.#active = false;
    }
  }

  async #copy(file, {directory, name, maxBytes, progress}) {
    let target;
    let created = false;
    try {
      const {root, persistent} = await persistentStorage();
      target = await root.getDirectoryHandle(directory, {create: true});
      try {
        await target.getFileHandle(name);
        throw new Error("The staging file already exists; release it before importing again.");
      } catch (error) {
        if (error.name !== "NotFoundError") throw error;
      }
      const handle = await target.getFileHandle(name, {create: true});
      created = true;
      const destination = await handle.createWritable();
      let bytes = 0;
      const measured = new TransformStream({
        transform(chunk, controller) {
          bytes += chunk.byteLength;
          if (bytes > maxBytes || bytes > file.size) {
            throw new Error("Import stream exceeded its declared byte limit.");
          }
          controller.enqueue(chunk);
          progress({bytes, total: file.size});
        }
      });
      await file.stream().pipeThrough(measured).pipeTo(destination);
      if (bytes !== file.size) throw new Error("The selected file ended before its declared length.");
      return {directory, name, bytes, persistent};
    } catch (error) {
      if (created) {
        try {
          await target.removeEntry(name);
        } catch (cleanupError) {
          if (cleanupError.name !== "NotFoundError") {
            throw new AggregateError([error, cleanupError], "Import failed and staging cleanup failed.");
          }
        }
      }
      throw error;
    }
  }
}
