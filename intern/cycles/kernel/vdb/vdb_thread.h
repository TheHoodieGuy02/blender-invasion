/*
 * Copyright 2016 Blender Foundation
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 * http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#ifndef __VDB_THREAD_H__
#define __VDB_THREAD_H__

#ifdef WITH_OPENVDB

CCL_NAMESPACE_BEGIN

struct Intersection;
struct KernelGlobals;
struct OpenVDBGlobals;
struct OpenVDBThreadData;
struct Ray;

class VDBVolume {
 public:
  static OpenVDBThreadData *thread_init(OpenVDBGlobals *vdb_globals);
  static void thread_free(OpenVDBThreadData *tdata);

  enum OpenVDB_SampleType {
    OPENVDB_SAMPLE_POINT = 0,
    OPENVDB_SAMPLE_BOX = 1,
  };

  static bool has_uniform_voxels(OpenVDBGlobals *vdb, int vdb_index);
  static bool sample(OpenVDBThreadData *vdb_thread,
                     int vdb_index,
                     float x,
                     float y,
                     float z,
                     float *r,
                     float *g,
                     float *b,
                     int sampling);
  static bool sample_index(OpenVDBThreadData *vdb_thread,
                           int vdb_index,
                           int x,
                           int y,
                           int z,
                           float *r,
                           float *g,
                           float *b);
  static bool intersect(OpenVDBThreadData *vdb_thread,
                        int vdb_index,
                        const Ray *ray,
                        float *isect);
  static bool march(OpenVDBThreadData *vdb_thread, int vdb_index, float *t0, float *t1);
};

class VDBThread {
 public:
  OpenVDBThreadData *data;
  VDBThread(OpenVDBGlobals *vdb)
  {
    data = VDBVolume::thread_init(vdb);
  }
  ~VDBThread()
  {
    VDBVolume::thread_free(data);
  }
};

CCL_NAMESPACE_END

#endif

#endif /* __VDB_THREAD_H__ */