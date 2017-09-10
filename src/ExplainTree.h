/*
 * Souffle - A Datalog Compiler
 * Copyright (c) 2017, The Souffle Developers. All rights reserved
 * Licensed under the Universal Permissive License v 1.0 as shown at:
 * - https://opensource.org/licenses/UPL
 * - <souffle root>/licenses/SOUFFLE-UPL.txt
 */

/************************************************************************
 *
 * @file ExplainTree.h
 *
 * Classes for storing a derivation tree
 *
 ***********************************************************************/

#pragma once

#include <sstream>
#include <string>
#include <vector>

namespace souffle {

class ScreenBuffer {
private:
    uint32_t width;   // width of the screen buffer
    uint32_t height;  // height of the screen buffer
    char* buffer;     // screen contents

public:
    // constructor
    ScreenBuffer(uint32_t w, uint32_t h) : width(w), height(h), buffer(nullptr) {
        assert(width > 0 && height > 0 && "wrong dimensions");
        buffer = new char[width * height];
        memset(buffer, ' ', width * height);
    }

    ~ScreenBuffer() {
        delete[] buffer;
    }

    // write into screen buffer at a specific location
    void write(uint32_t x, uint32_t y, const std::string& s) {
        assert(x >= 0 && x < width && "wrong x dimension");
        assert(y >= 0 && y < height && "wrong y dimension");
        assert(x + s.length() <= width && "string too long");
        for (size_t i = 0; i < s.length(); i++) {
            buffer[y * width + x + i] = s[i];
        }
    }

    std::string getString() {
        std::stringstream ss;
        print(ss);
        return ss.str();
    }

    // print screen buffer
    void print(std::ostream& os) {
        if (height > 0 && width > 0) {
            for (int i = height - 1; i >= 0; i--) {
                for (size_t j = 0; j < width; j++) {
                    os << buffer[width * i + j];
                }
                os << std::endl;
            }
        }
    }
};

/***
 * Abstract Class for a Proof Tree Node
 *
 */
class TreeNode {
protected:
    std::string txt;  // text of tree node
    uint32_t width;   // width of node (including sub-trees)
    uint32_t height;  // height of node (including sub-trees)
    int xpos;         // x-position of text
    int ypos;         // y-position of text

public:
    TreeNode(const std::string& t = "") : txt(t), width(0), height(0), xpos(0), ypos(0) {}
    virtual ~TreeNode() {}

    // get width
    uint32_t getWidth() const {
        return width;
    }

    // get height
    uint32_t getHeight() const {
        return height;
    }

    // place the node
    virtual void place(uint32_t xpos, uint32_t ypos) = 0;

    // render node in screen buffer
    virtual void render(ScreenBuffer& s) = 0;
};

/***
 * Concrete class
 */
class InnerNode : public TreeNode {
private:
    std::vector<std::unique_ptr<TreeNode>> children;
    std::string label;

public:
    InnerNode(const std::string& nodeText = "", const std::string& label = "")
            : TreeNode(nodeText), label(label) {}

    // add child to node
    void add_child(std::unique_ptr<TreeNode> child) {
        children.push_back(std::move(child));
    }

    // place node and its sub-trees
    void place(uint32_t x, uint32_t y) {
        // there must exist at least one kid
        assert(children.size() > 0 && "no children");

        // set x/y pos
        xpos = x;
        ypos = y;

        height = 0;

        // compute size of bounding box
        for (const std::unique_ptr<TreeNode>& k : children) {
            k->place(x, y + 2);
            x += k->getWidth() + 1;
            width += k->getWidth() + 1;
            height = std::max(height, k->getHeight());
        }
        height += 2;

        // text of inner node is longer than all its sub-trees
        if (width < txt.length()) {
            width = txt.length();
        }
    };

    // render node text and separator line
    void render(ScreenBuffer& s) {
        s.write(xpos + (width - txt.length()) / 2, ypos, txt);
        for (const std::unique_ptr<TreeNode>& k : children) {
            k->render(s);
        }
        std::string separator(width - label.length(), '-');
        separator += label;
        s.write(xpos, ypos + 1, separator);
    }
};

/***
 * Concrete class for leafs
 */
class LeafNode : public TreeNode {
public:
    LeafNode(const std::string& t = "") : TreeNode(t) {}

    // place leaf node
    void place(uint32_t x, uint32_t y) {
        xpos = x;
        ypos = y;
        width = txt.length();
        height = 1;
    }

    // render text of leaf node
    void render(ScreenBuffer& s) {
        s.write(xpos, ypos, txt);
    }
};

}  // end of namespace souffle