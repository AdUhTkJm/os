#ifndef EXT2_H
#define EXT2_H

#include "vfs.h"

/*
For ext2 format, see:
https://wiki.osdev.org/Ext2
*/

namespace os {

class ext2_inode : public os::inode_impl<ext2_inode> {
  
};

class ext2 : public fs {
public:
  ext2();
  ext2_inode *get() override;
  void erase(inode*) override;
};

}

#endif
