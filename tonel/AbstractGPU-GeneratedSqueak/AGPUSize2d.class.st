Class {
	#name : #AGPUSize2d,
	#pools : [
		'AGPUConstants'
	],
	#superclass : #ExternalStructure,
	#category : 'AbstractGPU-GeneratedSqueak'
}

{ #category : #'definition' }
AGPUSize2d class >> fields [
	"
	self defineFields
	"
    ^ #(
		(width 'ulong')
		(height 'ulong')
	)
]

