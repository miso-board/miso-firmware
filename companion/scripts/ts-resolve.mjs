// Lets a plain `node` script import the app's TypeScript modules.
//
// Node 24 strips types on its own, but it will not guess extensions the way Vite
// does, so `import "./layout"` fails. This hook appends `.ts` for relative
// specifiers that have no extension — enough to run src/lib modules directly and
// avoid pulling a bundler or a test runner into the project for one script.
import { registerHooks } from "node:module";

registerHooks({
  resolve(specifier, context, next) {
    if (/^\.{1,2}\//.test(specifier) && !/\.[a-z]+$/i.test(specifier)) {
      try {
        return next(specifier + ".ts", context);
      } catch {}
    }
    return next(specifier, context);
  },
});
