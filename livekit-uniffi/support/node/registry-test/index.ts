/*
 * Copyright 2026 LiveKit, Inc.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

// Installed from a registry as a real consumer would, so this exercises
// `files`, `exports`, and @ubjs/node's platform-package resolution — none of
// which a workspace link goes through.
import { buildVersion } from '@livekit/uniffi';

const version = buildVersion();
if (!version) {
  throw new Error('buildVersion() returned an empty value');
}
console.log(`Loaded @livekit/uniffi from registry; FFI version: v${version}`);
