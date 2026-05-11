/**
* Copyright 2023 Michael Pollind
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
#pragma once

namespace hpl {
    class Image;
    class cTextureManager;

    struct ImageResourceWrapper {
    public:
        ImageResourceWrapper();
        ImageResourceWrapper(cTextureManager* textureManager, Image* image, bool autoDestroy = true);
        ImageResourceWrapper(const ImageResourceWrapper& other) = delete;
        ImageResourceWrapper(ImageResourceWrapper&& other);
        ~ImageResourceWrapper();

        void SetAutoDestroyResource(bool autoDestroy);

        void operator=(const ImageResourceWrapper& other) = delete;
        void operator=(ImageResourceWrapper&& other);

        Image* GetImage() const { return m_image; }

    private:
        Image* m_image = nullptr;
        cTextureManager* m_textureManager = nullptr;
        bool m_autoDestroyResource = true;
    };
} // namespace hpl
