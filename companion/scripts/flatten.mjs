// Flattens Vite's single-file dist/index.html into the artifact-ready shape:
// the Artifact host supplies doctype/html/head/body, so the published file
// must carry only <title>, stylesheet links, <style> and <script> plus the
// body markup.
import { readFileSync, writeFileSync } from "node:fs";
import { resolve, dirname } from "node:path";
import { fileURLToPath } from "node:url";

const dist = resolve(dirname(fileURLToPath(import.meta.url)), "../dist");
const html = readFileSync(resolve(dist, "index.html"), "utf8");

const head = html.match(/<head[^>]*>([\s\S]*)<\/head>/i)?.[1] ?? "";
const body = html.match(/<body[^>]*>([\s\S]*)<\/body>/i)?.[1] ?? "";

// Keep everything from <head> except charset/viewport meta (the host adds those).
const headKept = head.replace(/<meta[^>]*>\s*/gi, "").trim();

writeFileSync(resolve(dist, "miso-companion.html"), headKept + "\n" + body.trim() + "\n");
console.log("dist/miso-companion.html written");
